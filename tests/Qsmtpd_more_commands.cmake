cmake_minimum_required(VERSION 2.8)

execute_process(COMMAND sleep 3)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "EHLO remote.example.org\r")
execute_process(COMMAND sleep 1)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "AUTH PLAIN AGEAYg==\r")
execute_process(COMMAND sleep 1)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "mail from:<> size=1025\r")
execute_process(COMMAND sleep 1)
execute_process(COMMAND ${CMAKE_COMMAND} -E echo "quit\r")
execute_process(COMMAND sleep 1)
