# Performance

This project does not promise a fixed end-to-end voice latency. Measure capture-stop → STT → routing/generation → device completion separately on the selected host/network. Existing skill matches bypass code generation; composition reuses JSON skills. A persistent WSS connection removes repeated audio/transcript transport, and native Lua avoids a network exchange per local loop iteration.

Local tiny Whisper and Groq turbo trade latency, accuracy and network dependence. Benchmark real microphone samples after model warmup. New code generation depends on the external coding model and cannot be guaranteed below five seconds.

Test results and build profiles are recorded in [TESTING.md](TESTING.md). Do not present compilation or simulated transport tests as hardware latency measurements.
