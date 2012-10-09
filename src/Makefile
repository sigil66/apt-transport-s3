
default: s3

clean:
	rm -f *.o s3+http

s3: s3+http_main.o s3+http.o connect.o
	 g++ -o s3+http s3+http.o s3+http_main.o connect.o -lapt-pkg -lapt-inst -lssl -lcrypto

%.o: %.cc
	 gcc  -I /usr/include -I./ -g -c $<

install: s3
	cp s3+http /usr/lib/apt/methods/
  ln -s /usr/lib/apt/methods/s3+http /usr/lib/apt/methods/s3