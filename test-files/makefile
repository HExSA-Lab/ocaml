OCAMLC = ocamlc 

ifeq ($(DUMP), 1)
DEBUG_OFF_FLAG = -runtime-variant d -dinstr  
else
DEBUG_OFF_FLAG = -runtime-variant d
endif

RM = rm -rf 

all: quicksort hello

quicksort: quicksort.ml 
	$(OCAMLC) $(DEBUG_OFF_FLAG) -o quicksort quicksort.ml

hello: hello.ml
	$(OCAMLC) $(DEBUG_OFF_FLAG) -o hello hello.ml

clean: 
	$(RM) quicksort hello *.cmi *.cmo *.~

