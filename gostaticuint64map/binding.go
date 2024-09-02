package gostaticuint64map

import (
	"fmt"
	"unsafe"
)

// #include <hipermap/static_uint64_map.h>
// #cgo LDFLAGS: -l hipermap -lstdc++
import "C"

type StaticUint64Map struct {
	dbPlace []byte
	db      *C.hm_u64map_database_t
}

func CompileKeyValues(keys, values []uint64) (*StaticUint64Map, error) {
	if len(keys) != len(values) {
		return nil, fmt.Errorf("len(keys) != len(values): %d != %d", len(keys), len(values))
	}

	dbPlaceSize := C.hm_u64map_db_place_size(C.uint(len(keys)))
	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_u64map_database_t
	hmErr := C.hm_u64map_compile(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.uint64_t)(unsafe.Pointer(&keys[0])),
		(*C.uint64_t)(unsafe.Pointer(&values[0])),
		C.uint(len(keys)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64map_compile failed: %d", hmErr)
	}
	return &StaticUint64Map{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}

func Compile(m map[uint64]uint64) (*StaticUint64Map, error) {
	if len(m) == 0 {
		return nil, fmt.Errorf("no keys")
	}

	keys := make([]uint64, 0, len(m))
	values := make([]uint64, 0, len(m))
	for k, v := range m {
		keys = append(keys, k)
		values = append(values, v)
	}

	return CompileKeyValues(keys, values)
}

func (m *StaticUint64Map) Find(key uint64) uint64 {
	return uint64(C.hm_u64map_find(m.db, C.uint64_t(key)))
}

func (m *StaticUint64Map) Benchmark(beginKey, endKey uint64) uint64 {
	return uint64(C.hm_u64map_benchmark(m.db, C.uint64_t(beginKey), C.uint64_t(endKey)))
}

func (m *StaticUint64Map) Serialize() ([]byte, error) {
	serSize := C.hm_u64map_serialized_size(m.db)
	ser := make([]byte, serSize)
	hmErr := C.hm_u64map_serialize(
		(*C.char)(unsafe.Pointer(&ser[0])),
		serSize,
		m.db,
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64map_serialize failed: %d", hmErr)
	}
	return ser, nil
}

func FromSerialized(buffer []byte) (*StaticUint64Map, error) {
	if len(buffer) == 0 {
		return nil, fmt.Errorf("empty buffer")
	}

	var dbPlaceSize C.size_t
	hmErr := C.hm_u64map_db_place_size_from_serialized(
		&dbPlaceSize,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64map_db_place_size_from_serialized failed: %d", hmErr)
	}

	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_u64map_database_t
	hmErr = C.hm_u64map_deserialize(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64map_deserialize failed: %d", hmErr)
	}

	return &StaticUint64Map{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}
