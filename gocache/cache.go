package gocache

import (
	"fmt"
	"unsafe"
)

// #include <hipermap/cache.h>
// #cgo LDFLAGS: -l hipermap
import "C"

type Cache struct {
	cachePlace []byte
	cache      *C.hm_cache_t
	capacity   int
}

func New(capacity, speed int) (*Cache, error) {
	var cachePlaceSize C.size_t
	hmErr := C.hm_cache_place_size(
		&cachePlaceSize,
		C.uint(capacity),
		C.int(speed),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_cache_place_size failed: %d", hmErr)
	}

	cachePlace := make([]byte, cachePlaceSize)
	var cache *C.hm_cache_t
	hmErr = C.hm_cache_init(
		(*C.char)(unsafe.Pointer(&cachePlace[0])),
		cachePlaceSize,
		&cache,
		C.uint(capacity),
		C.int(speed),
	)
	if hmErr != C.HM_SUCCESS {
		return nil, fmt.Errorf("hm_cache_init failed: %d", hmErr)
	}

	return &Cache{
		cachePlace: cachePlace,
		cache:      cache,
		capacity:   capacity,
	}, nil
}

func (c *Cache) Add(ip, value uint32) (existed, evicted bool, evictedIp, evictedValue uint32) {
	var cexisted, cevicted C.bool
	var cevictedIp, cevictedValue C.uint32_t
	C.hm_cache_add(
		c.cache,
		C.uint32_t(ip),
		C.uint32_t(value),
		&cexisted,
		&cevicted,
		&cevictedIp,
		&cevictedValue,
	)

	return bool(cexisted), bool(cevicted), uint32(cevictedIp), uint32(cevictedValue)
}

func (c *Cache) Remove(ip uint32) (existed bool, existedValue uint32) {
	var cexisted C.bool
	var cexistedValue C.uint32_t
	C.hm_cache_remove(
		c.cache,
		C.uint32_t(ip),
		&cexisted,
		&cexistedValue,
	)

	return bool(cexisted), uint32(cexistedValue)
}

func (c *Cache) Has(ip uint32) (exists bool, value uint32) {
	var cvalue C.uint32_t
	cexists := C.hm_cache_has(
		c.cache,
		C.uint32_t(ip),
		&cvalue,
	)
	return bool(cexists), uint32(cvalue)
}

func (c *Cache) Dump() []uint32 {
	ips := make([]uint32, c.capacity)
	ipsLen := C.size_t(c.capacity)
	C.hm_cache_dump(
		c.cache,
		(*C.uint32_t)(unsafe.Pointer(&ips[0])),
		&ipsLen,
	)
	return ips[:ipsLen]
}
