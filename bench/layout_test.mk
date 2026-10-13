# Run from the repository root after qmake has generated Makefile:
#   make -f Makefile -f bench/layout_test.mk -j8 check-layout
# Reuses the application's objects and libraries; only main is renamed for the test.
.PHONY: check-layout
check-layout: bench/layout_test
	./bench/layout_test

bench/layout_test_main.o: pdf_viewer/main.cpp main.o
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -Dmain=sioyek_application_main -o $@ $<

bench/layout_test.o: bench/layout_test.cpp pdf_viewer/document_view.h
	$(CXX) -c $(CXXFLAGS) $(INCPATH) -o $@ $<

bench/layout_test: bench/layout_test.o bench/layout_test_main.o $(filter-out main.o,$(OBJECTS))
	$(LINK) $(LFLAGS) -o $@ $^ $(LIBS)
