# run-node

Run **Node.js** version for your project without installing it system-wide.  
By default it uses the latest **LTS**, or you can pin to a specific version.

## Build
```bash
./build.sh
```


Or use [github-exec](https://github.com/zacuke/github-exec) to run this as part of a one-liner.

## Usage
```bash
./run-node server.js         # run with latest LTS
./run-node v20.11.1 server.js   # pin to specific version
```