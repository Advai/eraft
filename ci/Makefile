default: image build-test

BUILDER_IMAGE := $(or $(BUILDER_IMAGE),hub.docker.com/eraftio/eraft)

image:
	docker build -f Dockerfile -t $(BUILDER_IMAGE) .

build-dev:
	chmod +x build-test.sh
	docker run --rm -v $(realpath ..):/eraft hub.docker.com/eraftio/eraft:latest /eraft/ci/build-test.sh;
