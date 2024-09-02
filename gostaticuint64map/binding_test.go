package gostaticuint64map

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math/rand"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

func TestSimple(t *testing.T) {
	m := map[uint64]uint64{
		1: 2,
		2: 3,
	}

	db, err := Compile(m)
	require.NoError(t, err)

	require.Equal(t, uint64(2), db.Find(1))
	require.Equal(t, uint64(3), db.Find(2))
	require.Equal(t, uint64(0), db.Find(3))
	require.Equal(t, uint64(0), db.Find(0))

	ser, err := db.Serialize()
	require.NoError(t, err)
	t.Log("Serialized:", hex.EncodeToString(ser))

	db2, err := FromSerialized(ser)
	require.NoError(t, err)
	require.Equal(t, uint64(2), db2.Find(1))
	require.Equal(t, uint64(3), db2.Find(2))
	require.Equal(t, uint64(0), db2.Find(3))
	require.Equal(t, uint64(0), db2.Find(0))
}

func TestCompileFail(t *testing.T) {
	_, err := Compile(nil)
	require.ErrorContains(t, err, "no keys")

	_, err = Compile(map[uint64]uint64{})
	require.ErrorContains(t, err, "no keys")

	_, err = Compile(map[uint64]uint64{
		0: 1,
	})
	require.ErrorContains(t, err, "hm_u64map_compile failed: 4")

	_, err = Compile(map[uint64]uint64{
		1: 0,
	})
	require.ErrorContains(t, err, "hm_u64map_compile failed: 4")

	_, err = Compile(map[uint64]uint64{
		0: 0,
	})
	require.ErrorContains(t, err, "hm_u64map_compile failed: 4")

	_, err = CompileKeyValues(
		[]uint64{1, 1, 2},
		[]uint64{10, 100, 1000},
	)
	require.ErrorContains(t, err, "hm_u64map_compile failed: 4")

	// Make sure non-unique values are ok.
	_, err = CompileKeyValues(
		[]uint64{10, 100, 1000},
		[]uint64{1, 1, 2},
	)
	require.NoError(t, err)
}

func TestLarge(t *testing.T) {
	r := rand.New(rand.NewSource(200))

	const N = 10000
	m := make(map[uint64]uint64, N)
	for len(m) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := m[key]; has {
			continue
		}
		value := r.Uint64()
		if value == 0 {
			continue
		}
		m[key] = value
	}

	db, err := Compile(m)
	require.NoError(t, err)

	for k, v := range m {
		require.Equal(t, v, db.Find(k), k)
		for diff := int64(-10); diff <= 10; diff++ {
			key2 := k + uint64(diff)
			require.Equal(t, m[key2], db.Find(key2), key2)
		}
	}

	ser, err := db.Serialize()
	require.NoError(t, err)

	db2, err := FromSerialized(ser)
	require.NoError(t, err)

	for k, v := range m {
		require.Equal(t, v, db2.Find(k))
	}
}

func TestBenchmark(t *testing.T) {
	r := rand.New(rand.NewSource(200))

	const N = 10000
	m := make(map[uint64]uint64, N)
	for len(m) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := m[key]; has {
			continue
		}
		value := r.Uint64()
		if value == 0 {
			continue
		}
		m[key] = value
	}

	db, err := Compile(m)
	require.NoError(t, err)

	var someKey uint64
	for k := range m {
		someKey = k
		break
	}

	const M = 10000000

	got := db.Benchmark(someKey, someKey+M)

	want := uint64(0)
	for key := someKey; key != someKey+M; key++ {
		want ^= m[key]
	}

	require.Equal(t, want, got)
}

func BenchmarkLarge(b *testing.B) {
	r := rand.New(rand.NewSource(300))

	const N = 1500

	m := make(map[uint64]uint64, N)
	for len(m) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := m[key]; has {
			continue
		}
		value := r.Uint64()
		if value == 0 {
			continue
		}
		m[key] = value
	}

	db, err := Compile(m)
	require.NoError(b, err)

	var someKey uint64
	for k := range m {
		someKey = k
		break
	}

	b.ResetTimer()

	_ = db.Benchmark(someKey, someKey+uint64(b.N))
}

func FuzzFind(f *testing.F) {
	f.Add([]byte{1, 2, 3, 4, 6, 7, 8})

	f.Fuzz(func(t *testing.T, kvBytes []byte) {
		// Convert from bytes to pairs of uint64 (key and value).
		fill := 16 - len(kvBytes)%16
		kvBytes = append(kvBytes, make([]byte, fill)...)
		keys := make([]uint64, 0, len(kvBytes)/16)
		values := make([]uint64, 0, len(kvBytes)/16)
		for len(kvBytes) > 0 {
			keys = append(keys, binary.LittleEndian.Uint64(kvBytes[:8]))
			kvBytes = kvBytes[8:]
			values = append(values, binary.LittleEndian.Uint64(kvBytes[:8]))
			kvBytes = kvBytes[8:]
		}

		// Remove 0 and make keys unique.
		m := make(map[uint64]uint64, len(keys))
		for i, key := range keys {
			if key == 0 {
				continue
			}
			value := values[i]
			if value == 0 {
				continue
			}
			if _, has := m[key]; has {
				continue
			}
			m[key] = value
		}
		if len(m) == 0 {
			m[1] = 1
		}

		fmt.Println("m:", m)

		t1 := time.Now()
		db, err := Compile(m)
		require.NoError(t, err)
		require.Less(t, time.Since(t1), 10*time.Second)

		ser, err := db.Serialize()
		require.NoError(t, err)

		db2, err := FromSerialized(ser)
		require.NoError(t, err)

		for k, v := range m {
			require.Equal(t, v, db2.Find(k))
			for diff := int64(-10); diff <= 10; diff++ {
				key2 := k + uint64(diff)
				require.Equal(t, m[key2], db2.Find(key2), key2)
			}
		}
	})
}
