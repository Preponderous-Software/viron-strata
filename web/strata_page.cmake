# Finishes the page emcc wrote, after each link:
#   cmake -DHTML=<page> -DWASM=<.wasm> [-DDATA=<.data>] -DWEB=<repo web/ dir> -P web/strata_page.cmake
#
# - The loading shell (web/strata-kit/strata-loader.js, vendored from Strata)
#   and its bridge (web/strata-boot.js) go inline in place of their <script src>
#   tags, so the site is the same files it always was and the shell costs no
#   extra request before the engine starts downloading.
# - The download's size (the .wasm, plus a --preload-file package if any) goes
#   into data-bytes on <template id="strata-glue">: the arcade compresses on
#   the fly and sends no Content-Length, so the loader needs the size up front
#   to show real progress.
foreach(var HTML WASM WEB)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "strata_page.cmake: -D${var}= is required")
  endif()
endforeach()

file(READ "${HTML}" page)

foreach(pair "strata-kit/strata-loader\\.js|strata-kit/strata-loader.js" "strata-boot\\.js|strata-boot.js")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 pattern)
  list(GET pair 1 file)
  string(REGEX MATCH "<script src=\"?${pattern}\"?></script>" tag "${page}")
  if(tag STREQUAL "")
    message(FATAL_ERROR "strata_page.cmake: no <script src=\"${file}\"> tag in ${HTML}")
  endif()
  file(READ "${WEB}/${file}" body)
  string(FIND "${body}" "</script" closes)
  if(NOT closes EQUAL -1)
    message(FATAL_ERROR "strata_page.cmake: ${file} contains </script and cannot go inline")
  endif()
  string(REPLACE "${tag}" "<script>/* ${file} */\n${body}</script>" page "${page}")
endforeach()

file(SIZE "${WASM}" bytes)
if(DEFINED DATA AND NOT DATA STREQUAL "")
  file(SIZE "${DATA}" data_bytes)
  math(EXPR bytes "${bytes} + ${data_bytes}")
endif()
string(REGEX MATCH "data-bytes=\"?0\"?" stamp "${page}")
if(stamp STREQUAL "")
  message(FATAL_ERROR "strata_page.cmake: no data-bytes=\"0\" in ${HTML}")
endif()
string(REPLACE "${stamp}" "data-bytes=\"${bytes}\"" page "${page}")

file(WRITE "${HTML}" "${page}")
