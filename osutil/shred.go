package osutil

import (
	"os"
	"crypto/rand"
)

const nSteps = 10


func Shred(f string) error {
	fileInfo, err := os.Stat(f)
	if err != nil {
		return err
	}
	sz := fileInfo.Size()
	file, err := os.OpenFile(f, os.O_RDWR, 0755)
	if err != nil {
		return err
	}
	defer file.Close()

	data := make([]byte, sz)
	for i := 0; i < 10; i++ {
		_, err = rand.Read(data)
		if err != nil {
			return err
		}
		_, err = file.Write(data)
		if err != nil {
			return err
		}
	}

	err = os.Remove(f)
	if err != nil {
		return err
	}
	return nil
}
