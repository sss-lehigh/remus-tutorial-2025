FROM ubuntu:24.04

# Get latest software (standard ubuntu package manager)
RUN apt-get update -y
RUN apt-get upgrade -y
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential python3 cmake git man curl libnuma-dev librdmacm-dev libibverbs-dev screen lsb-release software-properties-common wget

# Get LLVM 18
RUN curl -O https://apt.llvm.org/llvm.sh
RUN chmod +x llvm.sh
RUN ./llvm.sh 18
RUN rm ./llvm.sh

# Start in the `/root` folder
WORKDIR "/root"
