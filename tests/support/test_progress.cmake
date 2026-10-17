# Keep these records OUTSIDE execute_process OUTPUT/ERROR_VARIABLE buffers.
function(sol_test_phase name)
    string(REGEX REPLACE "[^a-zA-Z0-9_./:-]" "_" phase "${name}")
    message("SOL_TEST_PROGRESS\tevent=phase\tphase=${phase}\tcompleted=0\tunit=operation")
endfunction()

function(sol_test_done name)
    string(REGEX REPLACE "[^a-zA-Z0-9_./:-]" "_" phase "${name}")
    message("SOL_TEST_PROGRESS\tevent=phase_done\tphase=${phase}\tcompleted=1\ttotal=1\tunit=operation")
endfunction()
