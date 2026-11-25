# Runs `ammio --check` on every example config. Usage:
#   cmake -DAMMIO=<path to ammio> -DDIR=<config dir> -P check_configs.cmake
file(GLOB configs "${DIR}/*.json")
if(NOT configs)
    message(FATAL_ERROR "no configs found in ${DIR}")
endif()
foreach(config ${configs})
    execute_process(COMMAND ${AMMIO} --check ${config} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${config} is not valid")
    endif()
endforeach()
