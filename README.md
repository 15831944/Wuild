# About Wuild
Wuild (derived from "wild build") is a distributed compilation system, inspired by Distcc project. Main goals:
- Cross-platform builds (for example, Linux guests and Windows hosts);
- Simplicity;
- Fast integration and usability.

Wuild is written in C++, using Ninja (<https://github.com/ninja-build>) as one of frontends. 

# Code structure and terminology
Platform directory contains platform core primitives to base on: sockets, threads, file utilities.  
CompilerInterface declares CompilerInvocation and LocalExecutorTask.  
Configs contains settings structures for other services.  
ConfiguredApplication integrates utility options.  
Main application logic contained in Modules directory:
- LocalExecutor is lib for running tasks on local host based on Ninja's SubprocessSet.  
- InvocationRewriter splits tool invocation into preprocess and compilation.  
- RemoteToolServer executes compilator on a host, and recives tasks for such execution;
- CoordinatorSever maintains list of RemoteToolServers
- RemoteToolClient sends compilation task to different ToolServers; ToolServers retrived from Coordinator.
- CompilerProxy is WIP.
Main application binaries holded in root directory.

# Installation
### Requirements 
- Cmake (tested 3.6)
- C++ compiler with C+\+14 and C+\+1z filesystem support (tested Gcc, Mingw 6.2 and Msvc 2015)
- System zlib is optional.

### Building
Just use cmake. If you wish, you could disable using system zlib using USE_SYSTEM_ZLIB=false cmake option.  
Installation is not ready yet, so you just get "Wuild*" binaries in cmake bin directory.

### Configuring
Suppose you have machine 192.168.0.1 (or "client") running Linux and Gcc toolchaing, and machine 192.168.0.2 (or "server") just the same configuration (but more CPU resources). Create "Wuild.ini" in home directory on each machine with these contents:
- Client:
```
[tools]
toolIds=gcc_cpp,gcc_c     ; comma-separated logical compiler names
gcc_cpp=/usr/bin/g++,g++
gcc_c=/usr/bin/gcc,gcc    ; comma-separated possible binaries names, first must be absolute

[toolClient]
coordinatorHost=192.168.56.2   ; ip or hostname
coordinatorPort=7767 
```
- Server
```
[tools]
toolIds=gcc_cpp,gcc_c     ; compiler names must be identical on client and server
gcc_cpp=/usr/bin/g++,g++
gcc_c=/usr/bin/gcc,gcc 

[coordinator]
listenPort=7767           ; this will be used to coordinate clients and tool server. 
                          ; for now, just use the same machine for tool server and coordinator.
[toolServer]
threadCount=8            ; set host CPU's used for compilation
listenHost=192.168.0.2    ; network ip or host name for incoming connections
listenPort=7765           ; should be different for coordinator's
coordinatorHost=localhost 
coordinatorPort=7767 
```

Then setup autorunning of WuildToolServer and WuildCoordinator at Server. For testing, just start them in console. 

### Running Wuild build
At this moment, only Ninja integration is supported. So, generate ```CMake -G 'Ninja' -DCMAKE_MAKE_PROGRAM=/path/to/WuildNinja``` for some project, and run WuildNinja just as ninja. If all done correctly, build will be distributed between tool servers.  
If not, see Thoubleshooting.

# Troubleshooting
- Try to enable more verbose logs in Wuild config, adding "logLevel=6" in top of ini-file. logLevel=7 may be too verbose.
- Try to call Ninja wuild frontend with -v -n arguments (verbose dry run). You should see preprocess and compile invocations separately.
- Try to run different tests, e.g ```TestAllConfigs -c test.cpp -o test.o ``` - it emulates gcc commandline interface, but distributes build.

# Licensing 
  Copyright (C) 2017 Smirnov Vladimir mapron1@gmail.com  
  Source code licensed under the Apache License, Version 2.0 (the "License");  
  You may not use this file except in compliance with the License.  
  You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0 or in [file](COPYING-APACHE-2.0.txt)  

  Unless required by applicable law or agreed to in writing, software  
  distributed under the License is distributed on an "AS IS" BASIS,  
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  
  See the License for the specific language governing permissions and  
  limitations under the License.h  
