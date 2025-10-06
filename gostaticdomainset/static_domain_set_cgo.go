//go:build !use_pure_gostaticdomainset
// +build !use_pure_gostaticdomainset

package gostaticdomainset

import (
	"fmt"
	"unsafe"
)

// #include <stdlib.h>
// #include <hipermap/static_domain_set.h>
import "C"

type StaticDomainSet struct {
	dbPlace []byte
	db      *C.hm_domain_database_t
}

// Compile builds a static domain set from a slice of domains.
// Domains must be ASCII and case-insensitive; empty strings are not allowed.
func Compile(domains []string) (*StaticDomainSet, error) {
	if len(domains) == 0 {
		return nil, ErrNoDomains
	}

	// Build C array of *C.char
	cstrs := make([]*C.char, len(domains))
	for i, s := range domains {
		if s == "" {
			return nil, ErrEmptyDomain
		}
		cstrs[i] = C.CString(s)
	}
	defer func() {
		for _, p := range cstrs {
			if p != nil {
				C.free(unsafe.Pointer(p))
			}
		}
	}()

	// Allocate db place
	dbPlaceSize := C.hm_domain_db_place_size((**C.char)(unsafe.Pointer(&cstrs[0])), C.uint(len(domains)))
	if dbPlaceSize == 0 {
		return nil, ErrBadValue
	}
	dbPlace := make([]byte, dbPlaceSize)

	// Compile
	var db *C.hm_domain_database_t
	hmErr := C.hm_domain_compile(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(**C.char)(unsafe.Pointer(&cstrs[0])),
		C.uint(len(domains)),
	)
	if hmErr != C.HM_SUCCESS {
		// Map known error codes to stable Go errors.
		switch hmErr {
		case C.HM_ERROR_BAD_ALIGNMENT:
			return nil, ErrBadAlignment
		case C.HM_ERROR_SMALL_PLACE:
			return nil, ErrSmallPlace
		case C.HM_ERROR_NO_MASKS:
			return nil, ErrNoDomains
		case C.HM_ERROR_BAD_VALUE:
			return nil, ErrBadValue
		case C.HM_ERROR_TOO_MANY_POPULAR_DOMAINS:
			return nil, ErrTooManyPopularDomains
		case C.HM_ERROR_FAILED_TO_CALIBRATE:
			return nil, ErrFailedToCalibrate
		case C.HM_ERROR_TOP_LEVEL_DOMAIN:
			return nil, ErrTopLevelDomain
		default:
			return nil, fmt.Errorf("hm_domain_compile failed: %d", int(hmErr))
		}
	}

	return &StaticDomainSet{dbPlace: dbPlace, db: db}, nil
}

// Find returns whether the domain (case-insensitive) is present.
func (m *StaticDomainSet) Find(domain string) (bool, error) {
	res := C.hm_domain_find(
		m.db,
		(*C.char)(unsafe.Pointer(unsafe.StringData(domain))),
		C.size_t(len(domain)),
	)
	switch res {
	case 1:
		return true, nil

	case 0:
		return false, nil

	default:
		return false, fmt.Errorf("find failed with code %d", res)
	}
}

// Serialize emits a portable buffer (same-endian) for the database.
func (m *StaticDomainSet) Serialize() ([]byte, error) {
	serSize := C.hm_domain_serialized_size(m.db)
	ser := make([]byte, serSize)
	hmErr := C.hm_domain_serialize(
		(*C.char)(unsafe.Pointer(&ser[0])),
		serSize,
		m.db,
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_domain_serialize failed: %d", hmErr)
	}
	return ser, nil
}

// FromSerialized reconstructs a StaticDomainSet from a serialized buffer.
func FromSerialized(buffer []byte) (*StaticDomainSet, error) {
	if len(buffer) == 0 {
		return nil, fmt.Errorf("empty buffer")
	}
	var dbPlaceSize C.size_t
	hmErr := C.hm_domain_db_place_size_from_serialized(
		&dbPlaceSize,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_domain_db_place_size_from_serialized failed: %d", hmErr)
	}
	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_domain_database_t
	hmErr = C.hm_domain_deserialize(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_domain_deserialize failed: %d", hmErr)
	}
	return &StaticDomainSet{dbPlace: dbPlace, db: db}, nil
}

// String returns a short summary of the database:
// number of domains, number of popular hashes, fill percentage, and db_place_size.
func (m *StaticDomainSet) String() string {
	if m == nil || m.db == nil {
		return "StaticDomainSet{empty}"
	}
	// Query counts directly from C to avoid serialized header coupling.
	buckets := int(C.hm_domain_buckets(m.db))
	popCount := int(C.hm_domain_popular_count(m.db))
	usedTotal := int(C.hm_domain_used_total(m.db))
	D := 16

	// Calculate fill percentage across all slots (buckets * D).
	capCells := buckets * D
	var fillPct float64
	if capCells > 0 {
		fillPct = float64(usedTotal) * 100.0 / float64(capCells)
	}

	usedBytes := int(C.hm_domain_serialized_size(m.db))
	header := int(C.hm_domain_header_bytes())
	table := int(C.hm_domain_table_bytes(m.db))
	popular := int(C.hm_domain_popular_bytes(m.db))
	blob := int(C.hm_domain_blob_bytes(m.db))
	return fmt.Sprintf("StaticDomainSet{domains=%d, popular_hashes=%d, fill=%.1f%%, used=%d (header=%d, popular=%d, table=%d, domains=%d)}",
		usedTotal, popCount, fillPct, usedBytes, header, popular, table, blob)
}

// Seed returns the internal hash seed used by the database calibration.
func (m *StaticDomainSet) Seed() uint32 {
	if m == nil || m.db == nil {
		return 0
	}
	return uint32(C.hm_domain_hash_seed(m.db))
}

// Allocated returns the size of the backing db_place buffer in bytes.
func (m *StaticDomainSet) Allocated() int {
	if m == nil {
		return 0
	}
	return len(m.dbPlace)
}
