.PHONY: test test-protocol inspect build

test: test-protocol
	poetry run python -m unittest discover -s tools -p 'test_*.py' -v

test-protocol:
	@bin=$$(mktemp /tmp/dg-coop-protocol.XXXXXX); \
	trap 'rm -f "$$bin"' EXIT; \
	$(CXX) -std=c++20 -Wall -Wextra -Werror tools/test_protocol.cpp -o "$$bin" && "$$bin"

inspect:
	@poetry run python tools/engine_discovery.py --exe "$(EXE)" --manifest "$(MANIFEST)" --inventory config/steam-19221447.json

build:
	bash build.sh
