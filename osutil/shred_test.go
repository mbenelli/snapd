package osutil

import (
	"log"
	"os"

	"io/ioutil"
	"path/filepath"

	"gopkg.in/check.v1"
)

type ShredSuite struct{}
var _ = check.Suite(&ShredSuite{})

const size = 1024

func (s *ShredSuite) TestShred(c *check.C) {
	d, err := os.MkdirTemp("", "test")
	if err != nil {
		log.Fatal(err)
	}
	f := filepath.Join(d, "randomfile")
	if err != nil {
		log.Fatal(err)
	}
	data := make([]byte, size)
	err = ioutil.WriteFile(f, data, 0666)
	if err != nil {
		log.Fatal(err)
	}
	err = Shred(f)
	c.Assert(err, check.IsNil)
}
