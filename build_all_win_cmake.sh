#!/bin/bash

#####
# Function to check for error.
#####
CHECKERR () {
    ERRCODE=$?
    if [[ ! $TEST -eq 0 ]]; then
	echo "Error $ERRCODE caught. Exit."
	exit $ERRCODE
    fi
}

#####
# Function to do the building.
#####
DOBUILD () {
    BTYPE="static"
    if [ x$SHARED = "xON" ]; then
	BTYPE="shared"
    fi

    if [ $pltfrm = "32" ]; then
	CMAKEGEN=$CMAKEGEN32
    else
	CMAKEGEN=$CMAKEGEN64
    fi

    NAME="$VER-NC4_$NC4-DAP_$DAP-$pltfrm-$BTYPE"
    BDIR="build_$NAME"
    echo "Building $NAME"

    rm -rf $BDIR
    mkdir $BDIR
    cd $BDIR
    CMAKE_PREFIX_PATH=$RESOURCE_DIR cmake .. -G"$CMAKEGEN" -DCPACK_PACKAGE_FILE_NAME=NetCDF-$NAME -DENABLE_TESTS=OFF -DBUILD_SHARED_LIBS=$SHARED -DENABLE_DAP=$DAP -DENABLE_NETCDF_4=$NC4
    CHECKERR

    cmake --build .
    CHECKERR

    cmake --build . --target package
    CHECKERR

    mv -f *.zip *.dmg *.exe ../$INSTALLDIRS
    cd ..
    echo "Finished building $NAME"

}


#####
# Set up platform-specific variables 
#####

unamestr=`uname | cut -d " " -f 1`
VER="4.2.x-snapshot"
INSTALLDIRS="packages"

case $unamestr in
    Darwin) echo "Configuring Darwin"
    CMAKEGEN32="Unix Makefiles"
    ;;
    MINGW32_NT-6.1) echo "Configuring MSYS/MinGW"
    CMAKEGEN32="Visual Studio 10"
    ;;
    *) echo "Unknown platform: $unamestr"
    exit 1
    ;;
esac

rm -rf $INSTALLDIRS
mkdir -p $INSTALLDIRS


#####
# 32-bit
#####
RESOURCE_DIR=/c/share/w32/static
PLTFRM="32"


###
# Static
###

SHARED=OFF

#
# Minimum
#

NC4="OFF"
DAP="OFF"
DOBUILD

#
# NetCDF-4
#

NC4="ON"
DAP="OFF"
DOBUILD

#
# NetCDF-4, DAP
# 

NC4="ON"
DAP="ON"
DOBUILD

###
# Shared
###

SHARED=ON
#
# Minimum
#

NC4="OFF"
DAP="OFF"
DOBUILD

#
# NetCDF-4
#

NC4="ON"
DAP="OFF"
DOBUILD

#
# NetCDF-4, DAP
# 

NC4="ON"
DAP="ON"
DOBUILD