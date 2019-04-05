include(vcpkg_common_functions)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ValveSoftware/openvr
    REF v1.0.16
    SHA512 967356563ba4232da5361510c7519d3058e09eced4571aadc00d8a75ab1f299a0aebda2b0b10b0ffb6c6a443fd718634d0c0103964e289961449c93e8d7b9d02
    HEAD_REF master
)

set(VCPKG_LIBRARY_LINKAGE dynamic)

if(WIN32) 
  if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(ARCH_PATH "win64")
  elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(ARCH_PATH "win32")
  endif()

  set(LIB_EXTENSION "lib")
  set(BIN_LIB_EXTENSION "dll")
  set(SYMBOLS_EXTENSION "pdb")
endif()

if(UNIX AND NOT APPLE)
   if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(ARCH_PATH "linux64")
  elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    set(ARCH_PATH "linux32")
  endif()

  set(LIB_EXTENSION "so")
  set(BIN_LIB_EXTENSION "so")
  set(SYMBOLS_EXTENSION "so.dbg")
endif()


message("Gettting arch path")
message(${ARCH_PATH})
file(MAKE_DIRECTORY
    ${CURRENT_PACKAGES_DIR}/lib
    ${CURRENT_PACKAGES_DIR}/bin
    ${CURRENT_PACKAGES_DIR}/debug/lib
    ${CURRENT_PACKAGES_DIR}/debug/bin
)
file(COPY ${SOURCE_PATH}/lib/${ARCH_PATH}/libopenvr_api.${LIB_EXTENSION} DESTINATION ${CURRENT_PACKAGES_DIR}/lib)
file(COPY ${SOURCE_PATH}/lib/${ARCH_PATH}/libopenvr_api.${LIB_EXTENSION} DESTINATION ${CURRENT_PACKAGES_DIR}/debug/lib)
file(COPY
    ${SOURCE_PATH}/bin/${ARCH_PATH}/libopenvr_api.${BIN_LIB_EXTENSION}
    ${SOURCE_PATH}/bin/${ARCH_PATH}/libopenvr_api.${SYMBOLS_EXTENSION}
    DESTINATION ${CURRENT_PACKAGES_DIR}/bin
)
file(COPY
    ${SOURCE_PATH}/bin/${ARCH_PATH}/libopenvr_api.${BIN_LIB_EXTENSION}
    ${SOURCE_PATH}/bin/${ARCH_PATH}/libopenvr_api.${SYMBOLS_EXTENSION}
    DESTINATION ${CURRENT_PACKAGES_DIR}/debug/bin
)
file(COPY ${SOURCE_PATH}/headers DESTINATION ${CURRENT_PACKAGES_DIR})
file(RENAME ${CURRENT_PACKAGES_DIR}/headers ${CURRENT_PACKAGES_DIR}/include)

file(COPY ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/openvr)
file(RENAME ${CURRENT_PACKAGES_DIR}/share/openvr/LICENSE ${CURRENT_PACKAGES_DIR}/share/openvr/copyright)
