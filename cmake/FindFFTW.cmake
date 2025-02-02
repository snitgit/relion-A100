# CMake helper to locate the needed libraries and headers
# for compilation of RELION binaries.
#

# Undo add by Snit
#set(FFTW_PATH /usr/local/cuda)
#set(FFTW_INCLUDES /usr/local/cuda/include)
#set(FFTW_LIBRARIES /usr/local/cuda/lib64)

# Undo add by Snit
#set(FFTW_LIB /usr/local/cuda/lib64)
#set(FFTW_INCLUDE /usr/local/cuda/include)


set(LIB_PATHFFT $ENV{FFTW_LIB})
set(INC_PATHFFT $ENV{FFTW_INCLUDE})

unset(FFTW_PATH CACHE)
unset(FFTW_INCLUDES CACHE)
unset(FFTW_LIBRARIES CACHE)

  
# Modify by Snit
if(DEFINED ENV{FFTW_INCLUDE})
    find_path(FFTW_PATH     NAMES fftw3.h PATHS ${INC_PATHFFT} )
    find_path(FFTW_INCLUDES NAMES fftw3.h PATHS ${INC_PATHFFT} )
  # find_path(FFTW_PATH     NAMES cufftw.h PATHS ${INC_PATHFFT} )
  # find_path(FFTW_INCLUDES NAMES cufftw.h PATHS ${INC_PATHFFT} )
else()
    find_path(FFTW_PATH     NAMES fftw3.h )
    find_path(FFTW_INCLUDES NAMES fftw3.h )
endif()

# Undo Modify by Snit
 find_library(_FFTW_SINGLE  NAMES fftw3f  PATHS ${LIB_PATHFFT} $ENV{FFTW_LIB} $ENV{FFTW_HOME} )
 find_library(_FFTW_DOUBLE  NAMES fftw3   PATHS ${LIB_PATHFFT} $ENV{FFTW_LIB} $ENV{FFTW_HOME} )

#find_library(_FFTW_SINGLE  NAMES libcufftw.so PATHS ${LIB_PATHFFT} $ENV{FFTW_LIB} $ENV{FFTW_HOME} )
#find_library(_FFTW_DOUBLE  NAMES libcufftw.so PATHS ${LIB_PATHFFT} $ENV{FFTW_LIB} $ENV{FFTW_HOME} )


if (FFTW_PATH AND FFTW_INCLUDES AND 
   (_FFTW_SINGLE OR NOT FFTW_FIND_REQUIRED_SINGLE) AND 
   (_FFTW_DOUBLE OR NOT FFTW_FIND_REQUIRED_DOUBLE))
   
	set(FFTW_FOUND TRUE)
	if (_FFTW_SINGLE)
		set(FFTW_LIBRARIES ${FFTW_LIBRARIES} ${_FFTW_SINGLE})
	endif()
	if (_FFTW_DOUBLE)
		set(FFTW_LIBRARIES ${FFTW_LIBRARIES} ${_FFTW_DOUBLE})
	endif()
	
	message(STATUS "Found FFTW")
	message(STATUS "FFTW_PATH: ${FFTW_PATH}")
	message(STATUS "FFTW_INCLUDES: ${FFTW_INCLUDES}")
	message(STATUS "FFTW_LIBRARIES: ${FFTW_LIBRARIES}")
	
else()

   set(FFTW_FOUND FALSE)
	
	#message(STATUS "FFTW_PATH: ${FFTW_PATH}")
	#message(STATUS "FFTW_INCLUDES: ${FFTW_INCLUDES}")
	#message(STATUS "_FFTW_SINGLE: ${_FFTW_SINGLE}")
	#message(STATUS "FFTW_FIND_REQUIRED_SINGLE: ${FFTW_FIND_REQUIRED_SINGLE}")
	#message(STATUS "_FFTW_DOUBLE: ${_FFTW_DOUBLE}")
	#message(STATUS "FFTW_FIND_REQUIRED_DOUBLE: ${FFTW_FIND_REQUIRED_DOUBLE}")
	
	if(NOT _FFTW_DOUBLE AND FFTW_FIND_REQUIRED_SINGLE)
		message(STATUS "Single-precision FFTW was required but NOT found")
  # add Snit
  		message(STATUS "Show FFTW path")
		message(STATUS "FFTW_PATH: ${FFTW_PATH}")
		message(STATUS "FFTW_INCLUDES: ${FFTW_INCLUDES}")
		message(STATUS "FFTW_LIBRARIES: ${FFTW_LIBRARIES}")
	endif()
	if(NOT _FFTW_SINGLE AND FFTW_FIND_REQUIRED_DOUBLE)
		message(STATUS "Double-precision FFTW was required but NOT found")
	endif()
	
endif()

if(FFTW_FIND_REQUIRED AND NOT FFTW_FOUND)
	message( FATAL_ERROR "The required FFTW libraries were not found." )
endif(FFTW_FIND_REQUIRED AND NOT FFTW_FOUND)
