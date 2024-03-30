images = jpeg420exif.jpg jpeg422jfif.jpg jpeg400jfif.jpg jpeg444.jpg
CFLAGS += -Wall

ifndef DEBUG
CFLAGS += -Ofast
endif

ifdef DEBUG
CFLAGS += -fsanitize=address
endif

$(images):
	wget "https://www.w3.org/MarkUp/Test/xhtml-print/20050519/tests/$@" -q

test: jpeg_decode.o test.o
	$(CC) $(CFLAGS) $^ -o $@ -lm

test_all: test $(images)
	rm -f *.tiff
	./test jpeg420exif.jpg
	./test jpeg422jfif.jpg
	./test jpeg400jfif.jpg
	./test jpeg444.jpg

python:
	python jpeg_python/setup.py build_ext -i

format:
	clang-format -i *.c *.h

clean:
	rm *.o *.tiff ./test
