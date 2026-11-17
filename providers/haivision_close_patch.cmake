# Compile a reviewed copy; leave the checksum-pinned upstream tree untouched.
# closeInternal holds m_ConnectionLock before destroying m_pCryptoControl, but
# asynchronous KMREQ/KMRSP handlers in 1.5.7 do not. Only these cold control
# handlers acquire the matching lock. No payload/ACK path is changed.
set(original_core "${haivision_SOURCE_DIR}/srtcore/core.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original_core}")
file(SHA256 "${original_core}" core_hash)
if(NOT core_hash STREQUAL "84eaa0260c838dfd333cbe46c0a8010928e9d08b3705618da3c994d7d1c6134c")
  message(FATAL_ERROR "Haivision close patch requires the exact pinned 1.5.7 core.cpp")
endif()
file(READ "${original_core}" patched_core)
string(REPLACE
  "            uint32_t srtdata_out[SRTDATA_MAXSIZE];\n            size_t   len_out = 0;"
  "            // SrtMux 1.5.7 compatibility fix: serialize KM processing with closeInternal.\n            ScopedLock crypto_lifetime(m_ConnectionLock);\n            if (m_bClosing || !m_pCryptoControl)\n                return true;\n            uint32_t srtdata_out[SRTDATA_MAXSIZE];\n            size_t   len_out = 0;"
  patched_core "${patched_core}")
string(REPLACE
  "        // KMRSP doesn't expect any following action\n        m_pCryptoControl->processSrtMsg_KMRSP"
  "        // SrtMux 1.5.7 compatibility fix: serialize KM processing with closeInternal.\n        ScopedLock crypto_lifetime(m_ConnectionLock);\n        if (m_bClosing || !m_pCryptoControl)\n            return true;\n        // KMRSP doesn't expect any following action\n        m_pCryptoControl->processSrtMsg_KMRSP"
  patched_core "${patched_core}")
# Native getStatus() treats CONNECTING with both flags false as broken. Publish
# m_bConnected before clearing m_bConnecting on successful caller completion.
# The existing connection lock still serializes this path with native close.
string(REPLACE
  "    // And, I am connected too.\n    m_bConnecting = false;\n\n"
  ""
  patched_core "${patched_core}")
string(REPLACE
  "    // Remove from rendezvous queue (in this particular case it's\n"
  "    // SrtMux 1.5.7 compatibility fix: publish connected before retiring connecting.\n    m_bConnecting = false;\n\n    // Remove from rendezvous queue (in this particular case it's\n"
  patched_core "${patched_core}")
set(patched_path "${CMAKE_CURRENT_BINARY_DIR}/haivision-patched/core.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/haivision-patched")
# configure_file avoids touching timestamps when the reviewed content is unchanged.
file(WRITE "${patched_path}.in" "${patched_core}")
configure_file("${patched_path}.in" "${patched_path}" COPYONLY)
file(SHA256 "${patched_path}" haivision_patch_hash)
set_source_files_properties("${original_core}" TARGET_DIRECTORY srt_virtual PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(srt_virtual PRIVATE "${patched_path}")
