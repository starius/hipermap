package gocache

import (
	"math/rand"
	"strconv"
	"testing"

	"github.com/stretchr/testify/require"
	"gitlab.com/starius/lru-gen/examples/int2string"
)

func TestCache(t *testing.T) {
	const capacity = 16
	const speed = 3
	c, err := New(capacity, speed)
	require.NoError(t, err)

	t.Log("init", c.Dump())

	for i := uint32(0); i < 16; i++ {
		existed, evicted, _, _ := c.Add(i, i)
		require.False(t, existed)
		require.False(t, evicted)
		t.Log("add", i, c.Dump())
	}

	for i := uint32(0); i < 16; i++ {
		has, value := c.Has(i)
		require.True(t, has, i)
		require.Equal(t, i, value)
		t.Log("has", i, c.Dump())
	}

	for i := uint32(16); i < 31; i++ {
		existed, evicted, evictedIp, evictedValue := c.Add(i, i)
		require.False(t, existed)
		require.True(t, evicted)
		require.Equal(t, i-16, evictedIp)
		require.Equal(t, i-16, evictedValue)
		t.Log("add", i, c.Dump())
	}

	for i := uint32(16); i < 31; i++ {
		has, value := c.Has(i)
		require.True(t, has, i)
		require.Equal(t, i, value)
		t.Log("has", i, c.Dump())
	}

	existed, existedValue := c.Remove(20)
	require.True(t, existed)
	require.Equal(t, uint32(20), existedValue)
	t.Log("remove", 20, c.Dump())

	for i := uint32(16); i < 31; i++ {
		has, value := c.Has(i)
		if i != 20 {
			require.True(t, has, i)
			require.Equal(t, i, value)
		} else {
			require.False(t, has, i)
		}
		t.Log("has", i, c.Dump())
	}
}

func TestCacheControl(t *testing.T) {
	const capacity = 64
	const maxIP = 150
	const maxValue = 1000000
	const valueSize = 1
	const speed = 4
	c, err := New(capacity, speed)
	require.NoError(t, err)

	control, err := int2string.NewLRU(capacity, capacity)
	require.NoError(t, err)

	// Run the same actions on both implementations to make
	// sure they behave the same.
	r := rand.New(rand.NewSource(111))

	for i := 0; i < 1000000; i++ {
		ip := r.Intn(maxIP)
		action := r.Intn(3)

		switch action {
		case 0:
			// Add.
			value := r.Intn(maxValue)
			existed1, _, _, _ := c.Add(uint32(ip), uint32(value))
			existed2 := control.Set(ip, strconv.Itoa(value), valueSize)
			require.Equal(t, existed2, existed1)

		case 1:
			// Check.
			has1, value1 := c.Has(uint32(ip))
			value2, has2 := control.Get(ip)
			require.Equal(t, has2, has1)
			if has2 {
				require.Equal(t, value2, strconv.Itoa(int(value1)))
			}

		case 2:
			// Remove.
			existed1, _ := c.Remove(uint32(ip))
			existed2 := control.DeleteIfExists(ip)
			require.Equal(t, existed2, existed1)
		}
	}
}
