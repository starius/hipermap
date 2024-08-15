package gostaticuint64set

import (
	"fmt"
	"unsafe"
)

// #include <hipermap/static_uint64_set.h>
// #cgo LDFLAGS: -l hipermap -lstdc++
import "C"

type StaticUint64Set struct {
	dbPlace []byte
	db      *C.hm_u64_database_t
}

func Compile(keys []uint64) (*StaticUint64Set, error) {
	if len(keys) == 0 {
		return nil, fmt.Errorf("no keys")
	}

	dbPlaceSize := C.hm_u64_db_place_size(C.uint(len(keys)))
	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_u64_database_t
	hmErr := C.hm_u64_compile(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.uint64_t)(unsafe.Pointer(&keys[0])),
		C.uint(len(keys)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64_compile failed: %d", hmErr)
	}
	return &StaticUint64Set{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}

func (m *StaticUint64Set) Find(key uint64) bool {
	return bool(C.hm_u64_find(m.db, C.uint64_t(key)))
}

func (m *StaticUint64Set) Benchmark(beginKey, endKey uint64) uint64 {
	return uint64(C.hm_u64_benchmark(m.db, C.uint64_t(beginKey), C.uint64_t(endKey)))
}

func (m *StaticUint64Set) Serialize() ([]byte, error) {
	serSize := C.hm_u64_serialized_size(m.db)
	ser := make([]byte, serSize)
	hmErr := C.hm_u64_serialize(
		(*C.char)(unsafe.Pointer(&ser[0])),
		serSize,
		m.db,
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64_serialize failed: %d", hmErr)
	}
	return ser, nil
}

func FromSerialized(buffer []byte) (*StaticUint64Set, error) {
	if len(buffer) == 0 {
		return nil, fmt.Errorf("empty buffer")
	}

	var dbPlaceSize C.size_t
	hmErr := C.hm_u64_db_place_size_from_serialized(
		&dbPlaceSize,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64_db_place_size_from_serialized failed: %d", hmErr)
	}

	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_u64_database_t
	hmErr = C.hm_u64_deserialize(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_u64_deserialize failed: %d", hmErr)
	}

	return &StaticUint64Set{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}
