package gostaticuint64set

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math/rand"
	"testing"

	"github.com/stretchr/testify/require"
)

func TestSimple(t *testing.T) {
	db, err := Compile(
		[]uint64{1, 2, 3},
	)
	require.NoError(t, err)

	require.True(t, db.Find(1))
	require.True(t, db.Find(2))
	require.True(t, db.Find(3))
	require.False(t, db.Find(4))
	require.False(t, db.Find(0))

	ser, err := db.Serialize()
	require.NoError(t, err)
	t.Log("Serialized:", hex.EncodeToString(ser))

	db2, err := FromSerialized(ser)
	require.NoError(t, err)
	require.True(t, db2.Find(1))
	require.True(t, db2.Find(2))
	require.True(t, db2.Find(3))
	require.False(t, db2.Find(4))
	require.False(t, db2.Find(0))
}

func TestCompileFail(t *testing.T) {
	_, err := Compile(
		[]uint64{},
	)
	require.ErrorContains(t, err, "no keys")

	_, err = Compile(
		[]uint64{0, 1, 2},
	)
	require.ErrorContains(t, err, "hm_u64_compile failed: 4")

	_, err = Compile(
		[]uint64{1, 1, 2},
	)
	require.ErrorContains(t, err, "hm_u64_compile failed: 4")
}

func TestCompileRejectsTooSmallDeclaredSizeOnMisalignedPlace(t *testing.T) {
	need := u64DBPlaceSizeForTest(1)
	require.Greater(t, need, 0)

	backing := make([]byte, need+64)
	hmErr := u64CompileWithPlaceSizeForTest(backing[1:], 1, []uint64{1})
	require.Equal(t, hmErrorSmallPlaceForTest(), hmErr)
}

func TestDeserializeRejectsTooSmallDeclaredSizeOnMisalignedPlace(t *testing.T) {
	db, err := Compile([]uint64{1, 2, 3})
	require.NoError(t, err)
	ser, err := db.Serialize()
	require.NoError(t, err)

	need, hmErr := u64DBPlaceSizeFromSerializedForTest(ser)
	require.Equal(t, 0, hmErr)
	require.Greater(t, need, 0)

	backing := make([]byte, need+64)
	hmErr = u64DeserializeWithPlaceSizeForTest(backing[1:], 1, ser)
	require.Equal(t, hmErrorSmallPlaceForTest(), hmErr)
}

func TestLarge(t *testing.T) {
	r := rand.New(rand.NewSource(200))

	const N = 10000
	keys := make([]uint64, 0, N)
	set := make(map[uint64]struct{}, len(keys))
	for len(keys) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := set[key]; has {
			continue
		}
		set[key] = struct{}{}
		keys = append(keys, key)
	}

	db, err := Compile(keys)
	require.NoError(t, err)

	for _, key := range keys {
		require.True(t, db.Find(key))
		for diff := int64(-10); diff <= 10; diff++ {
			key2 := key + uint64(diff)
			_, has := set[key2]
			require.Equal(t, has, db.Find(key2), key2)
		}
	}

	ser, err := db.Serialize()
	require.NoError(t, err)

	db2, err := FromSerialized(ser)
	require.NoError(t, err)

	for _, key := range keys {
		require.True(t, db2.Find(key))
	}
}

func TestBenchmark(t *testing.T) {
	r := rand.New(rand.NewSource(200))

	const N = 10000
	keys := make([]uint64, 0, N)
	set := make(map[uint64]struct{}, len(keys))
	for len(keys) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := set[key]; has {
			continue
		}
		set[key] = struct{}{}
		keys = append(keys, key)
	}

	db, err := Compile(keys)
	require.NoError(t, err)

	const M = 10000000

	got := db.Benchmark(keys[0], keys[0]+M)

	want := uint64(0)
	for key := keys[0]; key != keys[0]+M; key++ {
		if _, has := set[key]; has {
			want++
		}
	}

	require.Equal(t, want, got)
}

func BenchmarkLarge(b *testing.B) {
	r := rand.New(rand.NewSource(300))

	const N = 1500
	keys := make([]uint64, 0, N)
	set := make(map[uint64]struct{}, len(keys))
	for len(keys) < N {
		key := r.Uint64()
		if key == 0 {
			continue
		}
		if _, has := set[key]; has {
			continue
		}
		set[key] = struct{}{}
		keys = append(keys, key)
	}

	db, err := Compile(keys)
	require.NoError(b, err)

	b.ResetTimer()

	_ = db.Benchmark(keys[0], keys[0]+uint64(b.N))
}

func FuzzFind(f *testing.F) {
	f.Add([]byte{1, 2, 3, 4, 6, 7, 8})

	f.Fuzz(func(t *testing.T, keysBytes []byte) {
		// Convert from bytes to uint64.
		fill := 8 - len(keysBytes)%8
		keysBytes = append(keysBytes, make([]byte, fill)...)
		keys := make([]uint64, 0, len(keysBytes)/8)
		for len(keysBytes) > 0 {
			keys = append(keys, binary.LittleEndian.Uint64(keysBytes[:8]))
			keysBytes = keysBytes[8:]
		}

		// Remove 0 and make keys unique.
		set := make(map[uint64]struct{}, len(keys))
		keys2 := make([]uint64, 0, len(keys))
		for _, key := range keys {
			if key == 0 {
				continue
			}
			if _, has := set[key]; has {
				continue
			}
			set[key] = struct{}{}
			keys2 = append(keys2, key)
		}
		keys = keys2
		if len(keys) == 0 {
			keys = append(keys, 1)
			set[1] = struct{}{}
		}

		fmt.Println("keys:", keys)

		db, err := Compile(keys)
		require.NoError(t, err)

		ser, err := db.Serialize()
		require.NoError(t, err)

		db2, err := FromSerialized(ser)
		require.NoError(t, err)

		for _, key := range keys {
			require.True(t, db2.Find(key))
			for diff := int64(-10); diff <= 10; diff++ {
				key2 := key + uint64(diff)
				_, has := set[key2]
				require.Equal(t, has, db2.Find(key2), key2)
			}
		}
	})
}

func TestDeserializeRejectsInvalidBucketCount(t *testing.T) {
	// Serialized layout:
	// uint64 factor1
	// uint64 factor2
	// uint64 buckets
	// []uint64 hash_table
	//
	// buckets=1 underflows runtime mask computation and must be rejected.
	serialized := make([]byte, 4*8)
	binary.LittleEndian.PutUint64(serialized[0:8], 0xA6C3096657A14E89)
	binary.LittleEndian.PutUint64(serialized[8:16], 0x24F963569D05D92E)
	binary.LittleEndian.PutUint64(serialized[16:24], 1)
	binary.LittleEndian.PutUint64(serialized[24:32], 1)

	db, err := FromSerialized(serialized)
	if err == nil {
		_ = db.Find(1)
		t.Fatalf("expected deserialization failure for invalid bucket count")
	}
	require.ErrorContains(t, err, "hm_u64_db_place_size_from_serialized failed")
}
