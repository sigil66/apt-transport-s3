
default: all

clean:
	rm -f *.o s3+http s3+https

all: s3 s3s

s3: s3+http_main.o s3+http.o connect.o
	g++ -o s3+http s3+http.o s3+http_main.o connect.o -lapt-pkg -lapt-inst -lssl -lcrypto

s3s: s3+https.o connect.o
	g++ -o s3+https s3+https.o connect.o -lapt-pkg -lapt-inst -lssl -lcrypto -lcurl

%.o: %.cc
	gcc  -I /usr/include -I./ -g -c $<

install: s3
	cp s3+http /usr/lib/apt/methods/
	cp s3+https /usr/lib/apt/methods/
	ln -s /usr/lib/apt/methods/s3+http /usr/lib/apt/methods/s3

