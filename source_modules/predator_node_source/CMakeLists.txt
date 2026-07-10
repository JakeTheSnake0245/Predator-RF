cmake_minimum_required(VERSION 3.13)
project(predator_node_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(predator_node_source PRIVATE "src/")

# The remote-cockpit source module acts as the trusted Controller. When it
# forwards commands from the phone to a Predator-RF Device node, the Device's
# own tx.* guard enforces the RX-only policy. The Controller side must be
# able to forward ANY class, so we lift the local guard here.
target_compile_definitions(predator_node_source PRIVATE OPT_ALLOW_TX_COMMANDS)
