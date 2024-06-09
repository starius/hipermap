package gosm

import (
	"encoding/hex"
	"fmt"
	"testing"

	"github.com/stretchr/testify/require"
)

func TestCompile(t *testing.T) {
	_, err := Compile(
		[]uint32{0x01000000},
		[]uint8{8},
		[]uint64{0},
	)
	require.NoError(t, err)
}

func TestCompileFail(t *testing.T) {
	_, err := Compile(
		[]uint32{0x01000000},
		[]uint8{},
		[]uint64{0},
	)
	require.ErrorContains(t, err, "len(ips) != len(cidrPrefixes)")

	_, err = Compile(
		[]uint32{0x01000000},
		[]uint8{8},
		[]uint64{},
	)
	require.ErrorContains(t, err, "len(ips) != len(values)")

	_, err = Compile(
		[]uint32{0x01000000},
		[]uint8{8},
		[]uint64{0xFFFFFFFFFFFFFFFF},
	)
	require.ErrorContains(t, err, "hm_sm_compile failed: 4")

	_, err = Compile(
		[]uint32{0x01000001},
		[]uint8{8},
		[]uint64{1},
	)
	require.ErrorContains(t, err, "hm_sm_compile failed: 5")

	_, err = Compile(
		[]uint32{0x01000000},
		[]uint8{0},
		[]uint64{1},
	)
	require.ErrorContains(t, err, "hm_sm_compile failed: 5")

	_, err = Compile(
		[]uint32{0x01000000},
		[]uint8{33},
		[]uint64{1},
	)
	require.ErrorContains(t, err, "hm_sm_compile failed: 5")
}

func TestFind(t *testing.T) {
	testCases := [][]uint64{
		{
			10,
			0x01000000, 8, 10,
			0x02000000, 8, 20,
		},
		{
			20,
			0x01000000, 8, 10,
			0x00010000, 16, 11,
			0x02000000, 8, 20,
		},
		{
			30,
			0x01000000, 8, 10,
			0x00010000, 16, 11,
			0x02000000, 8, 10,
		},
		{
			40,
			0x01000000, 8, 10,
			0x00010000, 16, 11,
			0xFF000000, 8, 99,
		},
		{
			50,
			0x01000000, 8, 10,
			0x00010000, 16, 11,
			0x00012345, 32, 12,
			0xFF000000, 8, 99,
		},
		{
			60,
			0x80000000, 1, 100,
			0x90000000, 8, 200,
		},
	}

	for _, tc := range testCases {
		index := tc[0]
		tc = tc[1:]
		t.Run(fmt.Sprintf("%d", index), func(t *testing.T) {
			ips := []uint32{}
			cidrPrefixes := []uint8{}
			values := []uint64{}
			for len(tc) > 0 {
				ip, cidrPrefix, value := uint32(tc[0]), uint8(tc[1]), uint64(tc[2])
				tc = tc[3:]
				ips = append(ips, ip)
				cidrPrefixes = append(cidrPrefixes, cidrPrefix)
				values = append(values, value)
			}
			sm, err := Compile(ips, cidrPrefixes, values)
			require.NoError(t, err)

			ser, err := sm.Serialize()
			require.NoError(t, err)
			t.Log(hex.EncodeToString(ser))

			for _, ip := range sampleIps(ips) {
				require.Equal(t, findValue(ips, cidrPrefixes, values, ip), sm.Find(ip), fmt.Sprintf("%x", ip))
			}
		})
	}
}

func FuzzFind(f *testing.F) {
	ips := []uint32{
		0x01000000, 0x01110000, 0x02000000, 0x030F0000, 0x04000000,
		0x11000000, 0x11110000, 0x22000000, 0xA30F0000, 0xB4000000,
	}
	prefixes := []uint8{
		8, 16, 8, 16, 8,
		8, 16, 8, 16, 8,
	}
	values := []uint64{
		10, 20, 30, 40, 50,
		110, 120, 130, 140, 150,
	}

	f.Add(
		ips[0], ips[1], ips[2], ips[3], ips[4], ips[5], ips[6], ips[7], ips[8], ips[9],
		prefixes[0], prefixes[1], prefixes[2], prefixes[3], prefixes[4], prefixes[5], prefixes[6], prefixes[7], prefixes[8], prefixes[9],
		values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9],
	)

	f.Fuzz(func(t *testing.T,
		ip1, ip2, ip3, ip4, ip5, ip6, ip7, ip8, ip9, ip10 uint32,
		p1, p2, p3, p4, p5, p6, p7, p8, p9, p10 uint8,
		v1, v2, v3, v4, v5, v6, v7, v8, v9, v10 uint64) {

		ips := []uint32{ip1, ip2, ip3, ip4, ip5, ip6, ip7, ip8, ip9, ip10}
		cidrPrefixes := []uint8{p1, p2, p3, p4, p5, p6, p7, p8, p9, p10}
		values := []uint64{v1, v2, v3, v4, v5, v6, v7, v8, v9, v10}

		adjustInputs(ips, cidrPrefixes, values)

		t.Log(ips, cidrPrefixes, values)

		sm, err := Compile(ips, cidrPrefixes, values)
		require.NoError(t, err)

		ser, err := sm.Serialize()
		require.NoError(t, err)
		t.Log(hex.EncodeToString(ser))

		for _, ip := range sampleIps(ips) {
			require.Equal(t, findValue(ips, cidrPrefixes, values, ip), sm.Find(ip), fmt.Sprintf("%x", ip))
		}
	})
}

func adjustInputs(ips []uint32, cidrPrefixes []uint8, values []uint64) {
	// Adjust invalid errors to avoid compilation errors.
	for i, v := range values {
		if v == 0xFFFFFFFFFFFFFFFF {
			values[i] = 1
		}
	}
	for i, p := range cidrPrefixes {
		if p == 0 {
			cidrPrefixes[i] = 1
		}
		if p > 32 {
			cidrPrefixes[i] = 32
		}
	}
	for i, ip := range ips {
		p := cidrPrefixes[i]
		// Clear bits after prefix.
		ip = ip >> (32 - p)
		ip = ip << (32 - p)
		ips[i] = ip
	}
	// If there are duplicate ranges, enforce the same value.
	range2value := make(map[string]uint64)
	for i, ip := range ips {
		p := cidrPrefixes[i]
		v := values[i]
		s := fmt.Sprintf("%d/%d", ip, p)
		v0, has := range2value[s]
		if has {
			values[i] = v0
		} else {
			range2value[s] = v
		}
	}
}

func sampleIps(ips []uint32) []uint32 {
	res := []uint32{0, 1, 2, 0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF}
	for _, ip := range ips {
		res = append(res, ip-2, ip-1, ip, ip+1, ip+2)
	}
	return res
}

func findValue(ips []uint32, cidrPrefixes []uint8, values []uint64, testIp uint32) uint64 {
	res := uint64(0xFFFFFFFFFFFFFFFF)
	bestPrefix := -1
	for i := 0; i < len(ips); i++ {
		ip, cidrPrefix, value := ips[i], int(cidrPrefixes[i]), values[i]
		offset := 32 - cidrPrefix
		if cidrPrefix > bestPrefix && (testIp>>offset) == (ip>>offset) {
			bestPrefix = cidrPrefix
			res = value
		}
	}
	return res
}

func FuzzSerialize(f *testing.F) {
	ips := []uint32{
		0x01000000, 0x01110000, 0x02000000, 0x030F0000, 0x04000000,
		0x11000000, 0x11110000, 0x22000000, 0xA30F0000, 0xB4000000,
	}
	prefixes := []uint8{
		8, 16, 8, 16, 8,
		8, 16, 8, 16, 8,
	}
	values := []uint64{
		10, 20, 30, 40, 50,
		110, 120, 130, 140, 150,
	}

	f.Add(
		ips[0], ips[1], ips[2], ips[3], ips[4], ips[5], ips[6], ips[7], ips[8], ips[9],
		prefixes[0], prefixes[1], prefixes[2], prefixes[3], prefixes[4], prefixes[5], prefixes[6], prefixes[7], prefixes[8], prefixes[9],
		values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9],
	)

	f.Fuzz(func(t *testing.T,
		ip1, ip2, ip3, ip4, ip5, ip6, ip7, ip8, ip9, ip10 uint32,
		p1, p2, p3, p4, p5, p6, p7, p8, p9, p10 uint8,
		v1, v2, v3, v4, v5, v6, v7, v8, v9, v10 uint64) {

		ips := []uint32{ip1, ip2, ip3, ip4, ip5, ip6, ip7, ip8, ip9, ip10}
		cidrPrefixes := []uint8{p1, p2, p3, p4, p5, p6, p7, p8, p9, p10}
		values := []uint64{v1, v2, v3, v4, v5, v6, v7, v8, v9, v10}

		adjustInputs(ips, cidrPrefixes, values)

		sm, err := Compile(ips, cidrPrefixes, values)
		require.NoError(t, err)

		ser, err := sm.Serialize()
		require.NoError(t, err)

		sm2, err := FromSerialized(ser)
		require.NoError(t, err)

		require.Equal(t, sm2.dbPlace, sm2.dbPlace[:len(sm2.dbPlace)])
	})
}

func FuzzDeserialize(f *testing.F) {
	sample1, err := hex.DecodeString("1400000000000000ffffff80ffff1081ffff1181ffffff81ffffff82ffff0e83ffff0f83ffffff83ffffff84ffffff90ffff1091ffff1191ffffff91ffffffa1ffffffa2ffff0e23ffff0f23ffffff33ffffff34ffffff7fffffffffffffffff0a0000000000000014000000000000000a000000000000001e00000000000000ffffffffffffffff2800000000000000ffffffffffffffff3200000000000000ffffffffffffffff6e0000000000000078000000000000006e00000000000000ffffffffffffffff8200000000000000ffffffffffffffff8c00000000000000ffffffffffffffff9600000000000000ffffffffffffffff")
	require.NoError(f, err)

	sample2, err := hex.DecodeString("0400000000000000ffffff80ffffff81ffffff82ffffff7fffffffffffffffff0a000000000000001400000000000000ffffffffffffffff")
	require.NoError(f, err)

	sample3, err := hex.DecodeString("0600000000000000ffff0080ffff0180ffffff80ffffff81ffffff82ffffff7fffffffffffffffff0b00000000000000ffffffffffffffff0a000000000000001400000000000000ffffffffffffffff")
	require.NoError(f, err)

	f.Add(sample1)
	f.Add(sample2)
	f.Add(sample3)

	f.Fuzz(func(t *testing.T, ser []byte) {
		t.Log(hex.EncodeToString(ser))
		_, _ = FromSerialized(ser)
	})
}
