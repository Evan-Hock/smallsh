CC=gcc
CFLAGS=--std=gnu99
SOURCES=smallsh.c
EXENAME=smallsh

ZIPNAME=hocke_program3.zip
WHL=$(SOURCES) makefile README.txt

all:
	$(CC) $(CFLAGS) $(SOURCES) -o $(EXENAME)

debug:
	$(CC) $(CFLAGS) -g -Wall $(SOURCES) -o $(EXENAME)

# Does not clean up any damage you have done to your files with the smallsh program. Be responsible!
clean:
	rm -f $(EXENAME)

zip:
	zip $(ZIPNAME) $(WHL)
