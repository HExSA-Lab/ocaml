OCAMLC = ocamlc 

DEBUG_OFF_FLAG = -runtime-variant d -dinstr  

RM = rm -rf 

all: quicksort hello

quicksort: quicksort.ml 
	$(OCAMLC) $(DEBUG_OFF_FLAG) -o quicksort quicksort.ml

hello: hello.ml
	$(OCAMLC) $(DEBUG_OFF_FLAG) -o hello hello.ml

clean: 
	$(RM) quicksort hello *.cmi *.cmo *.~

