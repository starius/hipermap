//go:build use_pure_gostaticdomainset
// +build use_pure_gostaticdomainset

package gostaticdomainset

import (
	"errors"
	"strings"

	puregostaticdomainset "github.com/starius/hipermap/puregostaticdomainset"
)

type StaticDomainSet = puregostaticdomainset.StaticDomainSet

// Compile builds a static domain set using the pure Go implementation.
func Compile(domains []string) (*StaticDomainSet, error) {
	ds, err := puregostaticdomainset.Compile(domains)
	if err != nil {
		return nil, mapPureError(err)
	}
	return ds, nil
}

// FromSerialized reconstructs a StaticDomainSet from a serialized buffer.
func FromSerialized(buffer []byte) (*StaticDomainSet, error) {
	ds, err := puregostaticdomainset.FromSerialized(buffer)
	if err != nil {
		return nil, err
	}
	return ds, nil
}

func mapPureError(err error) error {
	switch {
	case errors.Is(err, puregostaticdomainset.ErrNoDomains):
		return ErrNoDomains
	case errors.Is(err, puregostaticdomainset.ErrEmptyDomain):
		return ErrEmptyDomain
	case errors.Is(err, puregostaticdomainset.ErrInvalidDomainChars):
		return ErrBadValue
	case errors.Is(err, puregostaticdomainset.ErrTopLevelDomain):
		return ErrTopLevelDomain
	case errors.Is(err, puregostaticdomainset.ErrTooManyPopularDomains):
		return ErrTooManyPopularDomains
	case errors.Is(err, puregostaticdomainset.ErrFailedToCalibrate):
		return ErrFailedToCalibrate
	case err != nil && strings.HasPrefix(err.Error(), "invalid length"):
		return ErrBadValue
	default:
		return err
	}
}
