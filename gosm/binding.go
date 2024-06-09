package gosm

import (
	"errors"
	"fmt"
	"unsafe"
)

// #include <hipermap/static_map.h>
// #cgo LDFLAGS: -l hipermap -lstdc++
import "C"

type StaticMap struct {
	dbPlace []byte
	db      *C.hm_sm_database_t
}

func Compile(ips []uint32, cidrPrefixes []uint8, values []uint64) (*StaticMap, error) {
	if len(ips) != len(cidrPrefixes) {
		return nil, errors.New("len(ips) != len(cidrPrefixes)")
	}
	if len(ips) != len(values) {
		return nil, errors.New("len(ips) != len(values)")
	}
	dbPlaceSize := C.hm_sm_db_place_size(C.uint(len(ips)))
	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_sm_database_t
	hmErr := C.hm_sm_compile(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.uint32_t)(unsafe.Pointer(&ips[0])),
		(*C.uint8_t)(unsafe.Pointer(&cidrPrefixes[0])),
		(*C.uint64_t)(unsafe.Pointer(&values[0])),
		C.uint(len(ips)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_sm_compile failed: %d", hmErr)
	}
	return &StaticMap{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}

func (m *StaticMap) Find(ip uint32) uint64 {
	return uint64(C.hm_sm_find(m.db, C.uint32_t(ip)))
}

func (m *StaticMap) Serialize() ([]byte, error) {
	serSize := C.hm_sm_serialized_size(m.db)
	ser := make([]byte, serSize)
	hmErr := C.hm_sm_serialize(
		(*C.char)(unsafe.Pointer(&ser[0])),
		serSize,
		m.db,
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_sm_serialize failed: %d", hmErr)
	}
	return ser, nil
}

func FromSerialized(buffer []byte) (*StaticMap, error) {
	if len(buffer) == 0 {
		return nil, fmt.Errorf("empty buffer")
	}

	var dbPlaceSize C.size_t
	hmErr := C.hm_sm_db_place_size_from_serialized(
		&dbPlaceSize,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_sm_db_place_size_from_serialized failed: %d", hmErr)
	}

	dbPlace := make([]byte, dbPlaceSize)
	var db *C.hm_sm_database_t
	hmErr = C.hm_sm_deserialize(
		(*C.char)(unsafe.Pointer(&dbPlace[0])),
		dbPlaceSize,
		&db,
		(*C.char)(unsafe.Pointer(&buffer[0])),
		C.size_t(len(buffer)),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_sm_deserialize failed: %d", hmErr)
	}

	return &StaticMap{
		dbPlace: dbPlace,
		db:      db,
	}, nil
}
