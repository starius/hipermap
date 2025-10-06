package gostaticdomainset

import "fmt"

// Public, comparable error values for Compile failures.
var (
	ErrNoDomains             = fmt.Errorf("no domains")
	ErrEmptyDomain           = fmt.Errorf("empty domain")
	ErrBadAlignment          = fmt.Errorf("db_place bad alignment")
	ErrSmallPlace            = fmt.Errorf("db_place too small")
	ErrBadValue              = fmt.Errorf("invalid domain list")
	ErrTooManyPopularDomains = fmt.Errorf("too many popular domains")
	ErrFailedToCalibrate     = fmt.Errorf("failed to calibrate")
	ErrTopLevelDomain        = fmt.Errorf("top-level domains are not supported")
)
