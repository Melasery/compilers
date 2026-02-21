LLVMCODE = optimizer
TEST = test
$(LLVMCODE): $(LLVMCODE).cpp
	g++ -g -I /usr/include/llvm-c-17/ -c $(LLVMCODE).cpp
	g++ $(LLVMCODE).o `llvm-config-17 --cxxflags --ldflags --libs core` -I /usr/include/llvm-c-17/ -o $@

clean: 
	rm -rf $(LLVMCODE)
	rm -rf *.o
