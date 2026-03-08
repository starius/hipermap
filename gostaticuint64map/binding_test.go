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

func TestSerializeDeserializeWithMisalignedBuffer(t *testing.T) {
	db, err := Compile(map[uint64]uint64{
		1: 10,
		2: 20,
		3: 30,
	})
	require.NoError(t, err)

	serSize := u64mapSerializedSizeForTest(db)
	require.Greater(t, serSize, 0)

	raw := make([]byte, serSize+1)
	misaligned := raw[1:]
	hmErr := u64mapSerializeIntoBufferForTest(db, misaligned)
	require.Equal(t, 0, hmErr)

	db2, err := FromSerialized(misaligned)
	require.NoError(t, err)
	for _, key := range []uint64{0, 1, 2, 3, 4, 100} {
		require.Equal(t, db.Find(key), db2.Find(key), key)
	}
}

func TestCompileRejectsTooSmallDeclaredSizeOnMisalignedPlace(t *testing.T) {
	need := u64mapDBPlaceSizeForTest(1)
	require.Greater(t, need, 0)

	backing := make([]byte, need+64)
	hmErr := u64mapCompileWithPlaceSizeForTest(backing[1:], 1, []uint64{1}, []uint64{2})
	require.Equal(t, hmErrorSmallPlaceForTest(), hmErr)
}

func TestDeserializeRejectsTooSmallDeclaredSizeOnMisalignedPlace(t *testing.T) {
	db, err := Compile(map[uint64]uint64{1: 2, 2: 3})
	require.NoError(t, err)
	ser, err := db.Serialize()
	require.NoError(t, err)

	need, hmErr := u64mapDBPlaceSizeFromSerializedForTest(ser)
	require.Equal(t, 0, hmErr)
	require.Greater(t, need, 0)

	backing := make([]byte, need+64)
	hmErr = u64mapDeserializeWithPlaceSizeForTest(backing[1:], 1, ser)
	require.Equal(t, hmErrorSmallPlaceForTest(), hmErr)
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

func TestDeserializeRejectsInvalidBucketCount(t *testing.T) {
	// Serialized layout:
	// uint64 factor1
	// uint64 factor2
	// uint64 buckets
	// uint64 dummy
	// []key_value_t hash_table
	//
	// buckets=1 underflows runtime mask computation and must be rejected.
	serialized := make([]byte, 4*8+16)
	binary.LittleEndian.PutUint64(serialized[0:8], 0xA6C3096657A14E89)
	binary.LittleEndian.PutUint64(serialized[8:16], 0x24F963569D05D92E)
	binary.LittleEndian.PutUint64(serialized[16:24], 1)
	binary.LittleEndian.PutUint64(serialized[24:32], 0)
	binary.LittleEndian.PutUint64(serialized[32:40], 1)
	binary.LittleEndian.PutUint64(serialized[40:48], 1)

	db, err := FromSerialized(serialized)
	if err == nil {
		_ = db.Find(1)
		t.Fatalf("expected deserialization failure for invalid bucket count")
	}
	require.ErrorContains(t, err, "hm_u64map_db_place_size_from_serialized failed")
}
