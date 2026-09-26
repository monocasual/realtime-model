add_executable(RealtimeModelTests
		TripleBufferTests.cpp
)

target_link_libraries(RealtimeModelTests PRIVATE RealtimeModel Catch2::Catch2WithMain)
