################################################################################ 
# Makefile  Makefile for Python Modules and other utilities
# Copyright Greenplum 2006-2011
################################################################################

# protect the default target for this file from the targets in Makefile.global
# and Makefile.thirdparty
default: all

subdir = tools/bin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

#
# SOURCE DIRECTORIES
#
SRC=$(CURDIR)
PYLIB_SRC=$(SRC)/pythonSrc


#
# INSTALL DIRECTORY
#
LIB_DIR=$(SRC)/lib
PYLIB_DIR=$(SRC)/ext

all: lockfile paramiko pycrypto pygresql stream pychecker psi unittest2

#
# Python Libraries
#


NETPERF_DIR=netperf-2.4.3
netperf:
	@echo "--- netperf"
	if [ ! -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src && gzip -d -c $(NETPERF_DIR).tar.gz | tar xvf - && cd $(NETPERF_DIR) && CC="$(CC)" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" ./configure) \
	fi
	cd $(SRC)/src/$(NETPERF_DIR) && $(MAKE)
	cp $(SRC)/src/$(NETPERF_DIR)/src/netperf $(SRC)/src/$(NETPERF_DIR)/src/netserver lib/

#
#  STREAM
#
STREAM_DIR=$(SRC)/src/stream
stream:
	@echo "--- stream"
	cd $(STREAM_DIR) && NO_M64=TRUE $(CC) $(CFLAGS) stream.c -o stream
	cp $(STREAM_DIR)/stream lib/stream 

#
# PyGreSQL
#
PYGRESQL_VERSION=4.0
PYGRESQL_DIR=PyGreSQL-$(PYGRESQL_VERSION)

pygresql:
	@echo "--- PyGreSQL"
	cd $(PYLIB_SRC)/$(PYGRESQL_DIR) && CC="$(CC)" CFLAGS="${CFLAGS}" LDFLAGS="-L$(top_builddir)/src/interfaces/libpq ${LDFLAGS}" python setup.py build
	mkdir -p $(PYLIB_DIR)/pygresql
	cp -r $(PYLIB_SRC)/$(PYGRESQL_DIR)/build/lib.*/* $(PYLIB_DIR)/pygresql
	touch $(PYLIB_DIR)/__init__.py

#
# PARAMIKO
#
PARAMIKO_VERSION=1.7.6-9
PARAMIKO_DIR=paramiko-$(PARAMIKO_VERSION)
paramiko:
	@echo "--- paramiko"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PARAMIKO_DIR).tar.gz
	cd $(PYLIB_SRC)/$(PARAMIKO_DIR)/ && python setup.py build
	cp -r $(PYLIB_SRC)/$(PARAMIKO_DIR)/build/lib/paramiko  $(PYLIB_DIR)/

#
# LOCKFILE
#
# note the awk commands are used to eliminate references to code in __init__.py
# that we don't use and also to insert a comment to tell pylint not to complain
# about these files since we are not in a position to correct those warnings.
#
LOCKFILE_VERSION=0.9.1
LOCKFILE_DIR=lockfile-$(LOCKFILE_VERSION)
LOCKFILE_SRC=$(PYLIB_SRC)/$(LOCKFILE_DIR)/build/lib/lockfile
LOCKFILE_DST=$(PYLIB_DIR)/lockfile
lockfile:
	@echo "--- lockfile"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(LOCKFILE_DIR).tar.gz
	cd $(PYLIB_SRC)/$(LOCKFILE_DIR)/ && python setup.py build
	mkdir -p $(PYLIB_DIR)/lockfile
	awk 'BEGIN{print "# pylint: disable-all"} /^if hasattr/ {exit} { print }' < $(LOCKFILE_SRC)/__init__.py    > $(LOCKFILE_DST)/__init__.py
	awk 'BEGIN{print "# pylint: disable-all"} { print }'                      < $(LOCKFILE_SRC)/pidlockfile.py > $(LOCKFILE_DST)/pidlockfile.py

#
# PYCRYPTO
#
PYCRYPTO_VERSION=2.0.1
PYCRYPTO_DIR=pycrypto-$(PYCRYPTO_VERSION)

pycrypto:
	@echo "--- pycrypto"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PYCRYPTO_DIR).tar.gz
	cd $(PYLIB_SRC)/$(PYCRYPTO_DIR)/ && CC="$(CC)" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" python setup.py build
	cp -r $(PYLIB_SRC)/$(PYCRYPTO_DIR)/build/lib.*/Crypto $(PYLIB_DIR)

#
# PSI
#
PSI_VERSION=0.3b2_gp
PSI_DIR=PSI-$(PSI_VERSION)

psi:
	@echo "--- psi"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PSI_DIR).tar.gz
	cd $(PYLIB_SRC)/$(PSI_DIR)/ && CC="$(CC)" CFLAGS="${CFLAGS}" LDFLAGS="${LDFLAGS}" python setup.py build
	cp -r $(PYLIB_SRC)/$(PSI_DIR)/build/lib.*/psi $(PYLIB_DIR)


#
# PYCHECKER
#
PYCHECKER_VERSION=0.8.18
PYCHECKER_DIR=pychecker-$(PYCHECKER_VERSION)

pychecker:
	@echo "--- pychecker"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(PYCHECKER_DIR).tar.gz
	cd $(PYLIB_SRC)/$(PYCHECKER_DIR)/ && python setup.py build
	cp -r $(PYLIB_SRC)/$(PYCHECKER_DIR)/build/lib/pychecker  $(PYLIB_DIR)/


#
# PYLINT
#

PYLINT_VERSION=0.21.0
PYLINT_DIR=pylint-$(PYLINT_VERSION)
LOGILAB_ASTNG_VERSION=0.20.1
LOGILAB_ASTNG_DIR=logilab-astng-$(LOGILAB_ASTNG_VERSION)
LOGILAB_COMMON_VERSION=0.50.1
LOGILAB_COMMON_DIR=logilab-common-$(LOGILAB_COMMON_VERSION)
PYLINT_PYTHONPATH=$(PYLIB_DIR):$(PYLIB_SRC)/$(PYLINT_DIR)/build/lib/

pylint:
	@echo "--- pylint"
	@cd $(PYLIB_SRC)/ && $(TAR) xzf $(PYLINT_DIR).tar.gz
	@cd $(PYLIB_SRC)/ && $(TAR) xzf $(LOGILAB_ASTNG_DIR).tar.gz
	@cd $(PYLIB_SRC)/ && $(TAR) xzf $(LOGILAB_COMMON_DIR).tar.gz
	@cd $(PYLIB_SRC)/$(PYLINT_DIR)/ && python setup.py build 1> /dev/null
	@cd $(PYLIB_SRC)/$(LOGILAB_ASTNG_DIR)/ && python setup.py build 1> /dev/null
	@cd $(PYLIB_SRC)/$(LOGILAB_COMMON_DIR)/ && python setup.py build 1> /dev/null
	@cp -r $(PYLIB_SRC)/$(LOGILAB_COMMON_DIR)/build/lib/logilab $(PYLIB_SRC)/$(PYLINT_DIR)/build/lib/
	@cp -r $(PYLIB_SRC)/$(LOGILAB_ASTNG_DIR)/build/lib/logilab $(PYLIB_SRC)/$(PYLINT_DIR)/build/lib/
	@touch $(PYLIB_SRC)/$(PYLINT_DIR)/build/lib/__init__.py
	@touch $(PYLIB_SRC)/$(PYLINT_DIR)/build/lib/logilab/__init__.py

UNITTEST2_VERSION=0.5.1
UNITTEST2_DIR=unittest2-${UNITTEST2_VERSION}
unittest2:
	@echo "--- unittest2"
	cd $(PYLIB_SRC)/ && $(TAR) xzf $(UNITTEST2_DIR).tar.gz
	cd $(PYLIB_SRC)/$(UNITTEST2_DIR)/ && python setup.py build
	cp -r $(PYLIB_SRC)/$(UNITTEST2_DIR)/build/lib/unittest2  $(PYLIB_DIR)/


PYTHON_FILES=`grep -l --exclude=Makefile --exclude=gplogfilter --exclude=gpcheckos --exclude=gpgenfsmap.py --exclude=throttlingD.py "/bin/env python" *`\
			 `grep -l "/bin/env python" $(SRC)/../sbin/*`\
			 `find ./gppylib -name "*.py"`\
			 `find $(SRC)/../sbin -name "*.py"`

checkcode: pylint
	@echo "Running pylint on management scripts..."
	@PYTHONPATH=$(PYTHONPATH):$(PYLINT_PYTHONPATH) $(PYLIB_SRC)/$(PYLINT_DIR)/bin/pylint -i y $(PYTHON_FILES) --rcfile=.rcfile > $(SRC)/../pylint.txt || true
	@echo -n "pylint_score=" > $(SRC)/../pylint_score.properties
	@grep "Your code has been rated at" $(SRC)/../pylint.txt | sed -e "s|Your .* \(.*\)/.*|\1|" >> $(SRC)/../pylint_score.properties

check-regress:
	@echo "Running regression tests..."
	@PYTHONPATH=$(SRC):$(SRC)/ext:$(PYTHONPATH) \
	gppylib/gpunit discover --verbose -s gppylib -p "test_regress*.py" 2> $(SRC)/../gpMgmt_testregress_results.log 1> $(SRC)/../gpMgmt_testregress_output.log

check:
	@echo "Running unit tests..."
	@PYTHONPATH=$(SRC):$(SRC)/ext:$(PYTHONPATH) \
	gppylib/gpunit discover --verbose -s gppylib -p "test_unit*.py" 2> $(SRC)/../gpMgmt_testunit_results.log 1> $(SRC)/../gpMgmt_testunit_output.log

solarisTest:
	@if [ `uname -s` = 'SunOS' ]; then \
		echo "SOLARIS" ; \
	fi

#
#EPYDOC
#

EPYDOC_VERSION=3.0.1
EPYDOC_DIR=epydoc-$(EPYDOC_VERSION)
EPYDOC_PYTHONPATH=$(PYLIB_DIR):$(PYLIB_SRC)/$(EPYDOC_DIR)/build/lib/

epydoc:
	@echo "--- epydoc"
	@cd $(PYLIB_SRC)/ && $(TAR) xzf $(EPYDOC_DIR).tar.gz
	@cd $(PYLIB_SRC)/$(EPYDOC_DIR)/ && python setup.py build 1> /dev/null

docs: epydoc
	@echo "Running epydoc on management scripts..."
	@PYTHONPATH=$(PYTHONPATH):$(EPYDOC_PYTHONPATH) $(PYLIB_SRC)/$(EPYDOC_DIR)/build/scripts-2.6/epydoc --config=.epydoc.config

clean :
	rm -rf $(PYLIB_SRC)/$(LOCKFILE_DIR)
	rm -rf $(PYLIB_SRC)/$(PARAMIKO_DIR)
	rm -rf $(PYLIB_SRC)/$(PYCRYPTO_DIR)
	rm -rf $(PYLIB_SRC)/$(PYLINT_DIR)
	rm -rf $(PYLIB_SRC)/$(LOGILAB_COMMON_DIR)
	rm -rf $(PYLIB_SRC)/$(LOGILAB_ASTNG_DIR)
	rm -rf $(PYLIB_SRC)/$(PYGRESQL_DIR)/build
	rm -rf $(PYLIB_SRC)/$(PSI_DIR)
	rm -rf $(PYLIB_SRC)/$(PYCHECKER_DIR)
	rm -rf $(PYLIB_SRC)/$(UNITTEST2_DIR)
	rm -rf $(STREAM_DIR)/stream lib/stream  
	if [ -f $(SRC)/src/$(NETPERF_DIR)/Makefile ]; then \
		(cd $(SRC)/src/$(NETPERF_DIR) && $(MAKE) clean) \
	fi
	rm -rf *.pyc lib/*.pyc

distclean: clean
	rm -rf $(NETPERF_DIR)    

SKIP_INSTALL=.epydoc.config|.gitignore|.p4ignore|.rcfile|Makefile|gpchecksubnetcfg|generate-greenplum-path.sh
install: all
	${INSTALL_SCRIPT} -d ${bindir}
	for files in `find * -maxdepth 0 -type f | grep -x -v -E "${SKIP_INSTALL}"`; do ${INSTALL_SCRIPT} $${files} ${bindir}; done
	${MAKE} -C gppylib $@
	${MAKE} -C ext $@
	for dirs in `find hawqpylib -type d` ; do ${INSTALL_SCRIPT} -d ${bindir}/hawqpylib/$${dirs}; done
	for files in `find hawqpylib -type f` ; do ${INSTALL_SCRIPT} $${files} ${bindir}/hawqpylib/; done
	${INSTALL_SCRIPT} -d ${bindir}/lib
	for files in `find lib -type f`; do ${INSTALL_SCRIPT} $${files} ${bindir}/lib; done
	unset LIBPATH; ./generate-greenplum-path.sh $(prefix) > ${prefix}/greenplum_path.sh
