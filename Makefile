# Makefile to build class 'helloworld~' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.

# library name
lib.name = myLooper~

# input source file (class name == source file basename)
class.sources = myLooper~.c

# all extra files to be included in binary distribution of the library
datafiles = myLooper~-help.pd myLooper~-meta.pd README.md

# include Makefile.pdlibbuilder from submodule directory 'pd-lib-builder'
PDLIBBUILDER_DIR=pd-lib-builder/
include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder
