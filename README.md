# PIQP

[![DOI](https://img.shields.io/badge/DOI-10.48550/arXiv.2304.00290-green.svg)](https://doi.org/10.48550/arXiv.2304.00290) [![Preprint](https://img.shields.io/badge/Preprint-arXiv-blue.svg)](https://arxiv.org/abs/2304.00290) [![Funding](https://img.shields.io/badge/Grant-NCCR%20Automation%20(51NF40180545)-90e3dc.svg)](https://nccr-automation.ch/)

PIQP is an embedded Proximal Interior Point Quadratic Programming solver, which can solve dense and sparse quadratic programs of the form

$$
\begin{aligned}
\min_{x} \quad & \frac{1}{2} x^\top P x + c^\top x \\
\text {s.t.}\quad & Ax=b, \\
& Gx \leq h, \\
& x_{lb} \leq x \leq x_{ub},
\end{aligned}
$$

Combining an infeasible interior point method with the proximal method of multipliers, the algorithm can handle ill-conditioned convex QP problems without the need for linear independence of the constraints.

## Features

* PIQP is written in header only C++ 14 leveraging the [Eigen](https://eigen.tuxfamily.org/index.php?title=Main_Page) library for vectorized linear algebra.
* Dense and sparse problem formulations are supported. For small dense problems, vectorized instructions and cache locality can be exploited more efficiently.
* Interface to Python with many more to follow.
* Open source under the BSD 2-Clause License.

## Interfaces

PIQP support a wide range of interfaces including
* C/C++ (with Eigen support)
* Python
* Matlab (soon)
* Julia (soon)
* Rust (soon)

## Credits

PIQP is developed by the following people:
* Roland Schwan (main developer)
* Yuning Jiang (methods and maths)
* Daniel Kuhn (methods and maths)
* Colin N. Jones (methods and maths)

All contributors are affiliated with the [Laboratoire d'Automatique](https://www.epfl.ch/labs/la/) and/or the [Risk Analytics and Optimization Chair](https://www.epfl.ch/labs/rao/) at [EPFL](https://www.epfl.ch/), Switzerland.

This work was supported by the [Swiss National Science Foundation](https://www.snf.ch/) under the [NCCR Automation](https://nccr-automation.ch/) (grant agreement 51NF40_180545).

PIQP is build on the following open-source libraries:
* [Eigen](https://eigen.tuxfamily.org/index.php?title=Main_Page) is the work horse under the hood, responsible for producing optimized numerical linear algebra code.
* [ProxSuite](https://github.com/Simple-Robotics/proxsuite) served as an inspiration for the code structure, and the instruction set optimized python bindings. We also utilize some utility functions, and helper macros for cleaner code.
* [SuiteSparse - LDL](https://github.com/DrTimothyAldenDavis/SuiteSparse) (modified version) is for solving linear systems in the sparse solver.
* [pybind11](https://github.com/pybind/pybind11) is used for generating the python bindings.
* [cpu_features](https://github.com/google/cpu_features) is used for run-time instruction set detection in the interface bindings.
* [OSQP](https://github.com/osqp/osqp) served as an inspiration for the C interface.

## Citing our Work

If you found PIQP useful in your scientific work, we encourage you to cite our accompanying paper:

```
@misc{schwan2023,
    author = {Schwan, Roland and Jiang, Yuning and Kuhn, Daniel and Jones, Colin N.},
    title = {PIQP: A Proximal Interior-Point Quadratic Programming Solver},
    year = {2023},
    eprint = {arXiv:2304.00290},
}
```

## License

PIQP is licensed under the BSD 2-Clause License.