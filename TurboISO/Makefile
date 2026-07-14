#compile parameters

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O3
#CFLAGS = -c -W -g  #-fprofile-arcs -ftest-coverage -coverage #-pg
#EXEFLAG = -g  #-fprofile-arcs -ftest-coverage -coverage #-pg #-O2

#add -lreadline -ltermcap if using readline or objs contain readline
library = #-lgcov -coverage

objdir = ./objs/
objfile = $(objdir)Graph.o

all: turbo.exe turbo_convert.exe

turbo.exe: main.cpp $(objfile) util.h Graph.h
	$(CXX) $(CXXFLAGS) -o turbo.exe main.cpp $(objfile) $(library)

$(objdir)Graph.o: Graph.cpp Graph.h
	$(CXX) $(CXXFLAGS) -c Graph.cpp -o $(objdir)Graph.o

turbo_convert.exe: convert_temporal.cpp
	$(CXX) $(CXXFLAGS) -o turbo_convert.exe convert_temporal.cpp

.PHONY: clean dist tarball test sumlines

clean:
	rm -f $(objdir)* turbo.exe turbo_convert.exe
dist: clean
	rm -f *.txt *.log

#tarball:
	#tar -czvf vf2.tar.gz main util match io graph Makefile README.md objs script vflib2

#test: main/test.o $(objfile)
	#$(CC) $(EXEFLAG) -o test main/test.cpp $(objfile) $(library)

#sumline:
	#bash script/sumline.sh
