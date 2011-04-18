CXXFLAGS=-Os -Wall -Werror
DEST=debian/tmp

all: exec_with_namespace with_exec_c.so

.PHONY: clean
clean:
	rm -f exec_with_namespace exec.o exec_scripting.o pipe.o with_exec_c.so

exec_with_namespace: exec_with_namespace.cpp exec_defs.hpp
	$(CXX) -static-libgcc $(CXXFLAGS) -g -o exec_with_namespace exec_with_namespace.cpp

exec.o: exec.cpp exec.hpp exec_defs.hpp
	$(CXX) -c $(CXXFLAGS) -fPIC -o $@ exec.cpp

pipe.o: pipe.cpp pipe.hpp
	$(CXX) -I/usr/include/lua5.1 -c $(CXXFLAGS) -fPIC -o $@ pipe.cpp

exec_scripting.o: exec_scripting.cpp exec.hpp pipe.hpp exec_defs.hpp
	$(CXX) -I/usr/include/lua5.1 -c $(CXXFLAGS) -fPIC -o $@ exec_scripting.cpp

with_exec_c.so: exec_scripting.o exec.o pipe.o
	$(CXX) -fPIC -shared -Wl,-z,defs $(CXXFLAGS) -o $@ $^ -llua5.1 -lluabind

