# Join script files with a newline after each, so a file ending in a // comment does not swallow the next:
# cmake -DOUT=out.js -DIN="a.js|b.js" -P joinjs.cmake
string (REPLACE "|" ";" files "${IN}")
file (WRITE "${OUT}" "")
foreach (f IN LISTS files)
    file (READ "${f}" s)
    file (APPEND "${OUT}" "${s}\n")
endforeach ()
