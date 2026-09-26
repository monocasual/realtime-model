add_executable(RealtimeModelTests
		RealtimeValueTests.cpp
)

target_link_libraries(RealtimeModelTests PRIVATE RealtimeModel Catch2::Catch2WithMain)
