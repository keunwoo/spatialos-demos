# SpatialOS empty project

This is an empty SpatialOS project that can be used as a slimmed-down target for
creating C++ workers from the [spatialos/CppBlankProject][CppBlankProject]
templates.

How to use this repository:

* Clone this repo.
* Clone `spatialos/CppBlankProject` into another directory.
* Use the `worker_create.sh` script from `spatialos/CppBlankProject` to add a
  worker to this project.
* Push your clone to your project repo.

## Why?

`CppBlankProject`, despite its name, is not truly blank.  It contains templates
for trivial managed and external C++ workers.  Instantiating a worker from the
templates is what the `CppBlankProject/worker_create.sh` script is for; however,
that script, amusingly enough, does not work correctly when you use it to create
a worker within the same repository.  You need to point it at (a directory
beneath) another target repo.

In ASCII art:

```
+----------------+                              +-------+
| CppBlankWorker | ----> worker_create.sh ----> |  ???  |
+----------------+                              +-------+
```

The purpose of _this_ repository is to be the target repo `???`.

Yes, you could just use another instance of the `CppBlankProject` repo as your
target, but do you really want to waste time and disk space building the
template workers whenever you build your project?  Probably not.

[CppBlankProject]: https://github.com/spatialos/CppBlankProject
