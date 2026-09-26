add_executable(RealtimeModelTests
	RealtimeDocumentTests.cpp
)

target_link_libraries(RealtimeModelTests PRIVATE RealtimeModel Catch2::Catch2WithMain)
