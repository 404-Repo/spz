import os
import pathlib

from setuptools import find_packages
from skbuild import setup

os.environ["PYTHON_PACKAGE_BUILD"] = "1"

HERE = pathlib.Path(__file__).parent

setup(
    name="pyspz",
    version="1.0.0",
    description="Python bindings for SPZ Gaussian Splat library",
    long_description=(HERE / "README.md").read_text(),
    long_description_content_type="text/markdown",
    author="Denis Avvakumov",
    author_email="denisavvakumov@gmail.com",
    url="https://github.com/404-Repo/pyspz",
    packages=find_packages(),
    install_requires=[],
    cmake_install_dir="pyspz",
    include_package_data=True,
    exclude_package_data={"": ["*.a", "*.h", "*.cmake"]},
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Operating System :: OS Independent",
        "License :: OSI Approved :: MIT License",
        "Intended Audience :: Developers",
        "Topic :: Software Development :: Libraries",
    ],
    license="MIT",
    python_requires=">=3.10",
    zip_safe=False,
    cmake_args=[
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
    ],
)
