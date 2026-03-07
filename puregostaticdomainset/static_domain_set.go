package puregostaticdomainset

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/zeebo/xxh3"
)

// Constants mirrored from the C implementation.
const (
	dSlots             = 16
	maxDomainLen       = 253
	magicV1Uint32      = 0x53444D48 // HMDS (v1)
	magicV2Uint32      = 0x32444D48 // HMD2 (v2)
	headerBytes        = 64         // round_up64(sizeof(hm_domain_database_t)) on 64-bit
	recordBytes        = 64         // sizeof(domains_table_record_t)
	blobTailPad        = 256        // safety pad after blob for comparisons
	maxPopularSuffixes = 256
)

// domainsTableRecord stores the fields needed at runtime and for serialization.
type domainsTableRecord struct {
	tags     [dSlots]uint16
	offsets  [dSlots]uint8 // offsets measured in units of dSlots from base
	baseOff  uint32        // domains_blob_offset in bytes
	used     uint16        // number of used slots at the front
	maxScans uint16        // maximum scans inside this bucket
}

// StaticDomainSet is a pure Go implementation (non-SIMD path).
type StaticDomainSet struct {
	fastModM uint64

	table   []domainsTableRecord
	tld     []domainsTableRecord
	popular []domainsTableRecord
	blob    []byte // concatenated, 16-byte aligned strings + 0x00 terminators + tail pad

	seed     uint32
	tldCount uint32
	popCount uint32
}

var (
	ErrNoDomains             = errors.New("no domains")
	ErrEmptyDomain           = errors.New("empty domain")
	ErrInvalidDomainChars    = errors.New("invalid domain characters")
	ErrTooManyPopularDomains = errors.New("too many popular domains")
	ErrFailedToCalibrate     = errors.New("failed to calibrate")
	// Deprecated: top-level domains are supported starting with serialization v2.
	ErrTopLevelDomain = errors.New("top-level domains are not supported")
)

// Compile builds a static domain set from a slice of domains.
// Domains must be ASCII in [A-Za-z0-9-._], case-insensitive; empty strings are not allowed.
func Compile(domains []string) (*StaticDomainSet, error) {
	if len(domains) == 0 {
		return nil, ErrNoDomains
	}

	// Preprocess: trim trailing '.', validate/Lowercase, enforce length, prune duplicates & subdomains.
	views, err := preprocessDomains(domains)
	if err != nil {
		return nil, err
	}
	if len(views) == 0 {
		return nil, ErrNoDomains
	}
	regular, tlds := splitRegularAndTLD(views)

	// Find popular suffixes (unique, sorted).
	popular := findPopularSuffixes(regular)
	if len(popular) > maxPopularSuffixes {
		return nil, ErrTooManyPopularDomains
	}

	// Calibrate seed + buckets and build previews without overflow.
	calib, ok := calibrateAndPreview(regular, popular)
	if !ok {
		return nil, ErrFailedToCalibrate
	}
	calib.tlds = tlds

	// Build the runtime database from preview.
	s := &StaticDomainSet{}
	if err := s.buildFromPreview(calib); err != nil {
		return nil, err
	}
	return s, nil
}

// Find returns whether the domain (case-insensitive) is present.
func (s *StaticDomainSet) Find(domain string) (bool, error) {
	if s == nil || len(s.table) == 0 {
		return false, nil
	}
	// Trim trailing dots.
	for len(domain) > 0 && domain[len(domain)-1] == '.' {
		domain = domain[:len(domain)-1]
	}
	if domain == "" {
		return false, findError(-1)
	}
	if len(domain) > maxDomainLen {
		return false, findError(-1)
	}
	// Lowercase and validate.
	lower := make([]byte, len(domain))
	if !domainToLower(lower, domain) {
		return false, findError(-1)
	}

	// TLD-first path: match the last label against tld table.
	tldStart := cutLastDomainLabel(lower)
	tldSuffix := lower[tldStart:]
	tldHash := hash64Span(tldSuffix, uint64(s.seed))
	tldTag := uint16((tldHash >> 32) & 0xFFFF)
	if scanRecordSet(s.tld, tldTag, tldSuffix, s.blob) {
		return true, nil
	}

	// Initial suffix: last two labels (or fewer if not enough labels).
	suffixStart := cutTwoLastDomainLabels(lower)
	suffix := lower[suffixStart:]

	// Hash the suffix.
	suffixHash := hash64Span(suffix, uint64(s.seed))

	// If suffix is popular, extend left while exact-suffix match exists.
	for suffixStart > 0 {
		popTag := uint16((suffixHash >> 32) & 0xFFFF)
		if !s.popularSuffixExists(popTag, suffix) {
			break
		}
		// extend to include previous label
		labelEnd := suffixStart - 1
		labelStart := cutLastDomainLabel(lower[:labelEnd])
		suffixHash = hash64Span(lower[labelStart:labelEnd], suffixHash)
		suffixStart = labelStart
		suffix = lower[suffixStart:]
	}

	// Find bucket via fastmod: (uint32)suffixHash % buckets.
	b := fastmodU32(uint32(suffixHash), s.fastModM, uint32(len(s.table)))
	rec := &s.table[b]

	// Scan within bucket upto maxScans, growing suffix leftwards each time.
	curStart := suffixStart
	curHash := suffixHash
	for scan := 1; ; scan++ {
		tag := uint16((curHash >> 32) & 0xFFFF)
		if scanTags(rec, tag, s.blob, curStart, lower) {
			return true, nil
		}
		if int(scan) >= int(rec.maxScans) {
			return false, nil
		}
		if curStart == 0 {
			return false, nil
		}
		// Grow left by one label.
		labelEnd := curStart - 1
		labelStart := cutLastDomainLabel(lower[:labelEnd])
		curHash = hash64Span(lower[labelStart:labelEnd], curHash)
		curStart = labelStart
	}
}

func findError(code int) error {
	return fmt.Errorf("find failed with code %d", code)
}

// Seed returns the hash seed selected during calibration.
func (s *StaticDomainSet) Seed() uint32 {
	if s == nil {
		return 0
	}
	return s.seed
}

// Serialize emits a buffer compatible with gostaticdomainset/C:
// [4-byte magic] [64-byte header] [tld records] [popular records] [table records] [blob]
func (s *StaticDomainSet) Serialize() ([]byte, error) {
	if s == nil || len(s.table) == 0 {
		return nil, fmt.Errorf("empty set")
	}
	tldBytes := len(s.tld) * recordBytes
	popBytes := len(s.popular) * recordBytes
	tblBytes := len(s.table) * recordBytes
	need := 4 + headerBytes + tldBytes + popBytes + tblBytes + len(s.blob)
	buf := make([]byte, need)
	// Magic
	binary.LittleEndian.PutUint32(buf[0:4], magicV2Uint32)

	// v2 header layout (64 bytes):
	//  0: fastmod_M (u64)
	//  8: buckets (u32)
	// 12: hash_seed (u32)
	// 16: popular_records (u32)
	// 20: popular_count (u32)
	// 24: tld_records (u32)
	// 28: tld_count (u32)
	// 32: domains_blob_size (u64)
	off := 4
	binary.LittleEndian.PutUint64(buf[off+0:], s.fastModM)
	binary.LittleEndian.PutUint32(buf[off+8:], uint32(len(s.table)))
	binary.LittleEndian.PutUint32(buf[off+12:], s.seed)
	binary.LittleEndian.PutUint32(buf[off+16:], uint32(len(s.popular)))
	binary.LittleEndian.PutUint32(buf[off+20:], s.popCount)
	binary.LittleEndian.PutUint32(buf[off+24:], uint32(len(s.tld)))
	binary.LittleEndian.PutUint32(buf[off+28:], s.tldCount)
	binary.LittleEndian.PutUint64(buf[off+32:], uint64(len(s.blob)))

	// TLD records
	at := 4 + headerBytes
	for i := range s.tld {
		writeRecord(buf[at:at+recordBytes], &s.tld[i])
		at += recordBytes
	}
	// Popular records
	for i := range s.popular {
		writeRecord(buf[at:at+recordBytes], &s.popular[i])
		at += recordBytes
	}
	// Table records
	for i := range s.table {
		writeRecord(buf[at:at+recordBytes], &s.table[i])
		at += recordBytes
	}
	// Blob
	copy(buf[at:], s.blob)
	return buf, nil
}

// FromSerialized reconstructs a StaticDomainSet from a compatible buffer.
func FromSerialized(buffer []byte) (*StaticDomainSet, error) {
	if len(buffer) < 4+headerBytes {
		return nil, fmt.Errorf("buffer too small")
	}
	magic := binary.LittleEndian.Uint32(buffer[0:4])

	var (
		fastM      uint64
		buckets    uint32
		seed       uint32
		popRecords uint32
		popCount   uint32
		tldRecords uint32
		tldCount   uint32
		blobBytes  uint64
		isV2       bool
	)

	hdr := buffer[4 : 4+headerBytes]
	switch magic {
	case magicV1Uint32:
		fastM = binary.LittleEndian.Uint64(hdr[0:8])
		buckets = binary.LittleEndian.Uint32(hdr[8:12])
		seed = binary.LittleEndian.Uint32(hdr[12:16])
		popRecords = binary.LittleEndian.Uint32(hdr[32:36])
		popCount = binary.LittleEndian.Uint32(hdr[36:40])
		tldRecords = 0
		tldCount = 0
		blobBytes = binary.LittleEndian.Uint64(hdr[48:56])
		isV2 = false
	case magicV2Uint32:
		fastM = binary.LittleEndian.Uint64(hdr[0:8])
		buckets = binary.LittleEndian.Uint32(hdr[8:12])
		seed = binary.LittleEndian.Uint32(hdr[12:16])
		popRecords = binary.LittleEndian.Uint32(hdr[16:20])
		popCount = binary.LittleEndian.Uint32(hdr[20:24])
		tldRecords = binary.LittleEndian.Uint32(hdr[24:28])
		tldCount = binary.LittleEndian.Uint32(hdr[28:32])
		blobBytes = binary.LittleEndian.Uint64(hdr[32:40])
		isV2 = true
	default:
		return nil, fmt.Errorf("bad magic")
	}

	if blobBytes%16 != 0 || blobBytes < blobTailPad {
		return nil, fmt.Errorf("invalid blob size")
	}
	if buckets == 0 {
		return nil, fmt.Errorf("invalid buckets")
	}
	if uint64(popCount) > uint64(popRecords)*dSlots {
		return nil, fmt.Errorf("popular count mismatch")
	}
	if uint64(tldCount) > uint64(tldRecords)*dSlots {
		return nil, fmt.Errorf("tld count mismatch")
	}

	// Prevent integer overflows before converting to int or allocating.
	maxInt := uint64(^uint(0) >> 1)
	if uint64(buckets) > maxInt || uint64(popRecords) > maxInt || uint64(tldRecords) > maxInt || blobBytes > maxInt {
		return nil, fmt.Errorf("serialized payload too large")
	}

	// Ranges
	at := 4 + headerBytes
	addU64 := func(a, b uint64) (uint64, bool) {
		if a > ^uint64(0)-b {
			return 0, false
		}
		return a + b, true
	}
	tldBytes := uint64(tldRecords) * recordBytes
	popBytes := uint64(popRecords) * recordBytes
	tblBytes := uint64(buckets) * recordBytes
	needAfterHdr, ok := addU64(tldBytes, popBytes)
	if !ok {
		return nil, fmt.Errorf("serialized payload size overflow")
	}
	needAfterHdr, ok = addU64(needAfterHdr, tblBytes)
	if !ok {
		return nil, fmt.Errorf("serialized payload size overflow")
	}
	needAfterHdr, ok = addU64(needAfterHdr, blobBytes)
	if !ok {
		return nil, fmt.Errorf("serialized payload size overflow")
	}
	if uint64(len(buffer)-at) < needAfterHdr {
		return nil, fmt.Errorf("buffer truncated")
	}

	s := &StaticDomainSet{
		seed:     seed,
		fastModM: fastM,
		popCount: popCount,
		tldCount: tldCount,
		tld:      make([]domainsTableRecord, tldRecords),
		popular:  make([]domainsTableRecord, popRecords),
		table:    make([]domainsTableRecord, buckets),
	}

	if isV2 {
		for i := 0; i < int(tldRecords); i++ {
			readRecord(buffer[at:at+recordBytes], &s.tld[i])
			at += recordBytes
		}
	}
	for i := 0; i < int(popRecords); i++ {
		readRecord(buffer[at:at+recordBytes], &s.popular[i])
		at += recordBytes
	}
	for i := 0; i < int(buckets); i++ {
		readRecord(buffer[at:at+recordBytes], &s.table[i])
		at += recordBytes
	}
	s.blob = make([]byte, blobBytes)
	copy(s.blob, buffer[at:at+int(blobBytes)])
	if uint32(s.popularSuffixCount()) != popCount {
		return nil, fmt.Errorf("popular count mismatch")
	}
	if uint32(s.tldSuffixCount()) != tldCount {
		return nil, fmt.Errorf("tld count mismatch")
	}
	return s, nil
}

// String returns a summary similar to the cgo version.
func (s *StaticDomainSet) String() string {
	if s == nil || len(s.table) == 0 {
		return "StaticDomainSet{empty}"
	}
	usedTotal := 0
	for i := range s.table {
		usedTotal += int(s.table[i].used)
	}
	usedTotal += int(s.tldCount)
	capCells := len(s.table) * dSlots
	var fillPct float64
	if capCells > 0 {
		fillPct = float64(usedTotal) * 100.0 / float64(capCells)
	}

	header := headerBytes
	tld := len(s.tld) * recordBytes
	popular := len(s.popular) * recordBytes
	table := len(s.table) * recordBytes
	blob := len(s.blob)
	used := 4 + header + tld + popular + table + blob
	return fmt.Sprintf("StaticDomainSet{domains=%d, tlds=%d, popular_hashes=%d, fill=%.1f%%, used=%d (header=%d, tld=%d, popular=%d, table=%d, domains=%d)}",
		usedTotal, s.tldCount, s.popCount, fillPct, used, header, tld, popular, table, blob)
}

// Allocated returns the total size of the materialized database in bytes.
func (s *StaticDomainSet) Allocated() int {
	if s == nil || len(s.table) == 0 {
		return 0
	}
	header := headerBytes
	popular := (len(s.popular) + len(s.tld)) * recordBytes
	table := len(s.table) * recordBytes
	blob := len(s.blob)
	return 4 + header + popular + table + blob
}

func (s *StaticDomainSet) popularSuffixCount() int {
	if s == nil {
		return 0
	}
	total := 0
	for i := range s.popular {
		total += int(s.popular[i].used)
	}
	return total
}

// tldSuffixCount returns how many TLD strings are stored in the set.
func (s *StaticDomainSet) tldSuffixCount() int {
	if s == nil {
		return 0
	}
	total := 0
	for i := range s.tld {
		total += int(s.tld[i].used)
	}
	return total
}

// -------- Helpers and builders --------

func writeRecord(dst []byte, r *domainsTableRecord) {
	// tags (32 bytes)
	for i := 0; i < dSlots; i++ {
		binary.LittleEndian.PutUint16(dst[i*2:], r.tags[i])
	}
	// offsets (16 bytes)
	off := 32
	copy(dst[off:off+dSlots], r.offsets[:])
	// domains_blob ptr (8 bytes) left zero
	// used (2), maxScans (2), baseOff (4)
	binary.LittleEndian.PutUint16(dst[56:], r.used)
	binary.LittleEndian.PutUint16(dst[58:], r.maxScans)
	binary.LittleEndian.PutUint32(dst[60:], r.baseOff)
}

func readRecord(src []byte, r *domainsTableRecord) {
	for i := 0; i < dSlots; i++ {
		r.tags[i] = binary.LittleEndian.Uint16(src[i*2:])
	}
	copy(r.offsets[:], src[32:48])
	r.used = binary.LittleEndian.Uint16(src[56:58])
	r.maxScans = binary.LittleEndian.Uint16(src[58:60])
	r.baseOff = binary.LittleEndian.Uint32(src[60:64])
}

// fastmodU32 computes (a % d) using a precomputed M = floor(2^64/d)+1.
func fastmodU32(a uint32, M uint64, d uint32) uint32 {
	low := M * uint64(a)
	// mul128_u32(highbits) equivalent: ((low * d) >> 64). In Go, emulate via 128-bit using big constant.
	// However, we can rely on the well-known identity used here: high 64 bits of low*d.
	// Implement using builtin 128-bit via math/bits Mul64 producing hi, lo of low*d.
	hi, _ := mul64(low, uint64(d))
	return uint32(hi)
}

// mul64 returns the high, low 64-bit of x*y.
func mul64(x, y uint64) (hi, lo uint64) {
	// use standard algorithm with 128-bit decomposition without math/bits (Go 1.24 has bits.Mul64 but avoid import)
	// (a<<32+b) * (c<<32+d) = a*c<<64 + (a*d+b*c)<<32 + b*d
	const mask32 = uint64(0xFFFFFFFF)
	x0 := x & mask32
	x1 := x >> 32
	y0 := y & mask32
	y1 := y >> 32
	w0 := x0 * y0
	t := x1*y0 + (w0 >> 32)
	w1 := t & mask32
	w2 := t >> 32
	w1 += x0 * y1
	hi = x1*y1 + w2 + (w1 >> 32)
	lo = (w1 << 32) | (w0 & mask32)
	return
}

func computeM(d uint32) uint64 {
	return ^uint64(0)/uint64(d) + 1
}

func roundUp16(x int) int { return (x + 15) &^ 15 }

func domainToLower(dst []byte, s string) bool {
	for i := 0; i < len(s); i++ {
		c := s[i]
		cl := c | 0x20
		isAlpha := (cl >= 'a' && cl <= 'z')
		ok := isAlpha || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
		if !ok {
			return false
		}
		if isAlpha {
			dst[i] = cl
		} else {
			dst[i] = c
		}
	}
	return true
}

func preprocessDomains(domains []string) ([]string, error) {
	items := make([]string, 0, len(domains))
	for _, s := range domains {
		if s == "" {
			return nil, ErrEmptyDomain
		}
		// Trim trailing dots
		for len(s) > 0 && s[len(s)-1] == '.' {
			s = s[:len(s)-1]
		}
		if s == "" || len(s) > maxDomainLen {
			return nil, fmt.Errorf("invalid length: %d", len(s))
		}
		b := make([]byte, len(s))
		if !domainToLower(b, s) {
			return nil, ErrInvalidDomainChars
		}
		items = append(items, string(b))
	}
	// Remove duplicates and prune subdomains.
	items = pruneSubdomains(items)
	return items, nil
}

// splitRegularAndTLD partitions domains into multi-label and single-label sets.
func splitRegularAndTLD(domains []string) ([]string, []string) {
	regular := make([]string, 0, len(domains))
	tlds := make([]string, 0, len(domains))
	for _, d := range domains {
		if strings.IndexByte(d, '.') == -1 {
			tlds = append(tlds, d)
		} else {
			regular = append(regular, d)
		}
	}
	return regular, tlds
}

// lessRevChar compares by reversed characters.
func lessRevChar(a, b string) bool {
	// Compare a.rbegin vs b.rbegin
	na, nb := len(a), len(b)
	i, j := na-1, nb-1
	for i >= 0 && j >= 0 {
		ca, cb := a[i], b[j]
		if ca < cb {
			return true
		}
		if ca > cb {
			return false
		}
		i--
		j--
	}
	// shorter suffix-only string comes first
	return na < nb
}

func isSubdomain(s, suf string) bool {
	if len(s) < len(suf) {
		return false
	}
	if !strings.HasSuffix(s, suf) {
		return false
	}
	if len(s) == len(suf) {
		return true
	}
	return s[len(s)-len(suf)-1] == '.'
}

func pruneSubdomains(domains []string) []string {
	if len(domains) == 0 {
		return domains
	}
	sort.Slice(domains, func(i, j int) bool { return lessRevChar(domains[i], domains[j]) })
	out := domains[:0]
	for _, s := range domains {
		if len(out) == 0 {
			out = append(out, s)
			continue
		}
		base := out[len(out)-1]
		if isSubdomain(s, base) {
			continue
		}
		out = append(out, s)
	}
	return out
}

func suffixLastKLabels(s string, k int) string {
	// returns last k labels (k>=2) or whole string
	end := len(s)
	cur := end
	for i := 0; i < k; i++ {
		pos := lastDotBefore(s, cur)
		if pos < 0 {
			return s
		}
		cur = pos
	}
	return s[cur+1:]
}

func lastDotBefore(s string, end int) int {
	for i := end - 1; i >= 0; i-- {
		if s[i] == '.' {
			return i
		}
	}
	return -1
}

func findPopularSuffixes(domains []string) []string {
	popular := make([]string, 0)
	if len(domains) == 0 {
		return popular
	}
	frontier := append([]string(nil), domains...)
	depth := 2
	for {
		buckets := make(map[string][]string)
		for _, s := range frontier {
			key := suffixLastKLabels(s, depth)
			buckets[key] = append(buckets[key], s)
		}
		next := make([]string, 0, len(frontier))
		for k, vec := range buckets {
			if len(vec) > dSlots {
				popular = append(popular, k)
				next = append(next, vec...)
			}
		}
		if len(next) == 0 {
			break
		}
		frontier = next
		depth++
	}
	sort.Strings(popular)
	// dedup
	popular = slicesUnique(popular)
	return popular
}

func slicesUnique(a []string) []string {
	if len(a) == 0 {
		return a
	}
	out := a[:1]
	for i := 1; i < len(a); i++ {
		if a[i] != out[len(out)-1] {
			out = append(out, a[i])
		}
	}
	return out
}

type previewRecord struct {
	tags     [dSlots]uint16
	items    []string
	maxScans uint16
}

type calibrationResult struct {
	tlds    []string
	popular []string
	buckets []previewRecord
	seed    uint32
}

func calibrateAndPreview(domains []string, popular []string) (calibrationResult, bool) {
	// start with minimum sane number of buckets
	bucketsNum := uint32(len(domains)/dSlots + 1)
	seed := uint32(0xA17F2344)
	const growSteps = 60
	const seedTries = 100
	for grow := 0; grow < growSteps; grow++ {
		for t := 0; t < seedTries; t++ {
			seed++
			buckets := make([]previewRecord, bucketsNum)
			if tryBuildRecords(domains, seed, buckets, popular) {
				return calibrationResult{popular: popular, buckets: buckets, seed: seed}, true
			}
		}
		// grow ~+5%
		bucketsNum2 := (bucketsNum * 21) / 20
		if bucketsNum2 <= bucketsNum {
			bucketsNum++
		} else {
			bucketsNum = bucketsNum2
		}
	}
	return calibrationResult{}, false
}

func tryBuildRecords(domains []string, seed uint32, buckets []previewRecord, popular []string) bool {
	M := computeM(uint32(len(buckets)))
	for _, s := range domains {
		bHash, tag, maxScans := buildChainedBucketAndTag(s, seed, popular)
		b := fastmodU32(bHash, M, uint32(len(buckets)))
		rec := &buckets[b]
		if len(rec.items) >= dSlots {
			return false
		}
		idx := len(rec.items)
		rec.tags[idx] = tag
		if maxScans > rec.maxScans {
			rec.maxScans = maxScans
		}
		rec.items = append(rec.items, s)
	}
	return true
}

func buildChainedBucketAndTag(s string, seed uint32, popular []string) (bucketHash uint32, tag uint16, maxScans uint16) {
	// Start from last two labels suffix.
	lower := []byte(s) // s already lowercased
	sufStart := cutTwoLastDomainLabels(lower)
	h := hash64Span(lower[sufStart:], uint64(seed))

	// Cut popular part if exact suffix matches in the provided popular list.
	sufAfterPop := sufStart
	for sufAfterPop > 0 {
		cur := string(lower[sufAfterPop:])
		if !stringInSlice(cur, popular) {
			break
		}
		// extend left by one label
		labelEnd := sufAfterPop - 1
		labelStart := cutLastDomainLabel(lower[:labelEnd])
		h = hash64Span(lower[labelStart:labelEnd], h)
		sufAfterPop = labelStart
	}

	bucketHash = uint32(h)

	// Now chain remaining labels to compute final tag and max scans.
	hf := h
	cur := sufAfterPop
	scans := uint16(1)
	for cur > 0 {
		labelEnd := cur - 1
		labelStart := cutLastDomainLabel(lower[:labelEnd])
		hf = hash64Span(lower[labelStart:labelEnd], hf)
		cur = labelStart
		scans++
	}
	tag = uint16((hf >> 32) & 0xFFFF)
	maxScans = scans
	return
}

func stringInSlice(s string, arr []string) bool {
	// arr is small (<=256), linear scan is fine
	for _, v := range arr {
		if v == s {
			return true
		}
	}
	return false
}

func (s *StaticDomainSet) buildFromPreview(cal calibrationResult) error {
	s.seed = cal.seed
	// Sizes for blob: tld first, then popular, then buckets; each string
	// rounded to 16; plus tail pad.
	blobSize := 0
	for _, sv := range cal.tlds {
		blobSize += roundUp16(len(sv) + 1)
	}
	for _, sv := range cal.popular {
		blobSize += roundUp16(len(sv) + 1)
	}
	for _, b := range cal.buckets {
		for _, it := range b.items {
			blobSize += roundUp16(len(it) + 1)
		}
	}
	blobSize += blobTailPad

	s.blob = make([]byte, blobSize)
	s.tld = make([]domainsTableRecord, (len(cal.tlds)+dSlots-1)/dSlots)
	s.popular = make([]domainsTableRecord, (len(cal.popular)+dSlots-1)/dSlots)
	s.table = make([]domainsTableRecord, len(cal.buckets))
	s.fastModM = computeM(uint32(len(s.table)))
	s.popCount = uint32(len(cal.popular))
	s.tldCount = uint32(len(cal.tlds))

	// Build TLD table.
	cur := 0
	for r := 0; r < len(s.tld); r++ {
		rec := &s.tld[r]
		rec.baseOff = uint32(cur)
		base := cur
		for i := 0; i < dSlots; i++ {
			idx := r*dSlots + i
			if idx >= len(cal.tlds) {
				break
			}
			sv := cal.tlds[idx]
			h := hash64Span([]byte(sv), uint64(s.seed))
			tag := uint16((h >> 32) & 0xFFFF)
			offUnits := (cur - base) / dSlots
			if offUnits > 255 {
				return fmt.Errorf("tld offset overflow")
			}
			rec.offsets[rec.used] = uint8(offUnits)
			rec.tags[rec.used] = tag
			copy(s.blob[cur:], sv)
			cur += len(sv)
			s.blob[cur] = 0
			cur++
			pad := roundUp16(cur) - cur
			if pad > 0 {
				cur += pad
			}
			rec.used++
		}
		if rec.used > 0 {
			rec.maxScans = 1
		}
	}

	// Build popular table
	for r := 0; r < len(s.popular); r++ {
		rec := &s.popular[r]
		rec.baseOff = uint32(cur)
		base := cur
		for i := 0; i < dSlots; i++ {
			idx := r*dSlots + i
			if idx >= len(cal.popular) {
				break
			}
			sv := cal.popular[idx]
			// Compute tag as in find: chain labels from last two labels leftwards.
			sufStart := cutTwoLastDomainLabels([]byte(sv))
			h := hash64Span([]byte(sv)[sufStart:], uint64(s.seed))
			cur2 := sufStart
			for cur2 > 0 {
				labelEnd := cur2 - 1
				labelStart := cutLastDomainLabel([]byte(sv)[:labelEnd])
				h = hash64Span([]byte(sv)[labelStart:labelEnd], h)
				cur2 = labelStart
			}
			tag := uint16((h >> 32) & 0xFFFF)
			offUnits := (cur - base) / dSlots
			if offUnits > 255 {
				return fmt.Errorf("popular offset overflow")
			}
			rec.offsets[rec.used] = uint8(offUnits)
			rec.tags[rec.used] = tag
			// Copy string + NUL, then round to 16
			copy(s.blob[cur:], sv)
			cur += len(sv)
			s.blob[cur] = 0
			cur++
			pad := roundUp16(cur) - cur
			if pad > 0 {
				// zero already; just advance
				cur += pad
			}
			rec.used++
		}
	}

	// Build buckets
	for b := 0; b < len(s.table); b++ {
		rec := &s.table[b]
		prev := cal.buckets[b]
		rec.used = uint16(len(prev.items))
		rec.maxScans = prev.maxScans
		rec.baseOff = uint32(cur)
		base := cur
		for i := 0; i < len(prev.items); i++ {
			offUnits := (cur - base) / dSlots
			if offUnits > 255 {
				return fmt.Errorf("bucket offset overflow")
			}
			rec.offsets[i] = uint8(offUnits)
			rec.tags[i] = prev.tags[i]
			it := prev.items[i]
			copy(s.blob[cur:], it)
			cur += len(it)
			s.blob[cur] = 0
			cur++
			pad := roundUp16(cur) - cur
			if pad > 0 {
				cur += pad
			}
		}
	}
	// Tail pad already reserved by blobSize; ensure cur <= len(blob)-blobTailPad
	if cur+blobTailPad > len(s.blob) {
		return fmt.Errorf("internal blob size mismatch")
	}
	return nil
}

func (s *StaticDomainSet) popularSuffixExists(tag uint16, suffix []byte) bool {
	return scanRecordSet(s.popular, tag, suffix, s.blob)
}

func scanTags(rec *domainsTableRecord, tag uint16, blob []byte, suffixStart int, lower []byte) bool {
	used := int(rec.used)
	if used == 0 {
		return false
	}
	sfxLen := len(lower) - suffixStart
	suffix := lower[suffixStart:]
	base := int(rec.baseOff)
	for i := 0; i < used; i++ {
		if rec.tags[i] != tag {
			continue
		}
		pos := base + int(rec.offsets[i])*dSlots
		if pos+sfxLen >= len(blob) {
			continue
		}
		if bytes.Equal(blob[pos:pos+sfxLen], suffix) && blob[pos+sfxLen] == 0 {
			return true
		}
	}
	return false
}

// scanRecordSet scans all records for (tag, suffix) exact-string matches.
func scanRecordSet(records []domainsTableRecord, tag uint16, suffix []byte, blob []byte) bool {
	sfxLen := len(suffix)
	for r := range records {
		rec := &records[r]
		if rec.used == 0 {
			continue
		}
		for i := 0; i < int(rec.used); i++ {
			if rec.tags[i] != tag {
				continue
			}
			pos := int(rec.baseOff) + int(rec.offsets[i])*dSlots
			if pos+sfxLen >= len(blob) {
				continue
			}
			if bytes.Equal(blob[pos:pos+sfxLen], suffix) && blob[pos+sfxLen] == 0 {
				return true
			}
		}
	}
	return false
}

func cutLastDomainLabel(b []byte) int {
	// return start offset of last label in b (exclusive end)
	for i := len(b) - 1; i >= 0; i-- {
		if b[i] == '.' {
			return i + 1
		}
	}
	return 0
}

func cutTwoLastDomainLabels(b []byte) int {
	end := len(b)
	// find last dot
	p := -1
	for i := end - 1; i >= 0; i-- {
		if b[i] == '.' {
			p = i
			break
		}
	}
	if p < 0 {
		return 0
	}
	// find previous dot before p
	for i := p - 1; i >= 0; i-- {
		if b[i] == '.' {
			return i + 1
		}
	}
	return 0
}

func hash64Span(b []byte, seed uint64) uint64 {
	return xxh3.HashSeed(b, seed)
}
