# abm-gen: generating type descriptions from C++ headers

`abm-gen` writes the `abmio::TypeTraits` specialisations of a simulator's agent and behaviour types
(`docs/INTEGRATION.md`, "How a simulator describes its types") from the simulator's C++ headers, so that a
model's types are described without anyone writing their visit functions by hand. It is milestone 1's issue
#9; issue #10 validates it on BioDynaMo.

## What it produces

For every agent or behaviour class it finds, one specialisation:

```cpp
template <>
struct abmio::TypeTraits<bdm::Cell> {
  static constexpr const char* kName = "bdm::Cell";
  static constexpr uint32_t kVersion = 1;
  using Base = bdm::Agent;
  template <class V>
  static void Visit(V& v) {
    v(Field("position").Unit("um"), &bdm::Cell::position_);
    v(Field("tractor_force"), &bdm::Cell::tractor_force_);
    v(Field("diameter").Unit("um"), &bdm::Cell::diameter_);
    // ...
  }
};
```

all in one header per model, which the model's build includes.

## How it finds the types

`abm-gen` is written in Python and parses the headers with Clang (`libclang`, packaged on conda-forge and
PyPI), using the project's own compile flags, taken from `compile_commands.json` or given on the command line.
It keeps the parser's record of macro uses:

- a class is an **agent** if `BDM_AGENT_HEADER(class, base, version)` appears in its body, and a
  **behaviour** if `BDM_BEHAVIOR_HEADER` does;
- the **qualified name** comes from the parser; the **base type** from the macro, checked against the class's
  base specifier; the **schema version** from the macro's class version, which BioDynaMo already increments
  when a class's members change.

The macro names are configuration: another simulator names its own.

## Members

- Data members are visited in declaration order; static members are skipped.
- A member's **field name** is its name without BioDynaMo's trailing underscore: `position_` becomes
  `position`.
- Members marked **transient** with `//!` after the declaration, such as an agent's lock, are left out:
  they are never stored or sent, and some of them, such as `Spinlock`, could not be described anyway. The
  parser does not always attach trailing comments to declarations, so `abm-gen` reads the marker from the
  source line.
- **Roles** default to all three (*checkpoint*, *exchange*, *export*). A side file can narrow them per field.

## Types: the customisation point

`abm-gen` does not interpret member types. It emits a pointer to each member, and the compiler works out the
member's shape through `ShapeOf` in `abmio/schema.h`: arithmetic types, fixed arrays, `std::array`,
`std::vector`, `std::string`, described types and lists of pointers to described types are known.

A simulator's own types are declared once, in its adapter, by specialising `abmio::FieldTraits`. For
BioDynaMo:

| Type | Declared as |
| --- | --- |
| `Real3` (`MathArray<real_t, 3>`) | an array of three floating-point elements |
| `AgentUid` | a reference to an agent, as a 64-bit agent identifier |
| `AgentPointer<T>` | a reference to an agent, as a 64-bit agent identifier |
| `InlineVector<Behavior*, N>` | a polymorphic list of behaviours |

`real_t` needs nothing: it is `double` or `float`, whichever BioDynaMo was built with. A member of a type
neither known nor declared fails to compile, at the generated line that names the field.

## Access to private members

BioDynaMo's members are private. One line added to its `BDM_AGENT_HEADER` and `BDM_BEHAVIOR_HEADER` macros
befriends the generated specialisations; user models need no change. This belongs to BioDynaMo's step 3,
which brings abm-io in.

## Units and roles: the side file

C++ headers carry no units, so an optional side file per simulator or model supplies them, and narrows roles
where needed:

```yaml
bdm::Cell:
  position: {unit: um}
  diameter: {unit: um}
  volume: {unit: um3}
```

A field without an entry has no unit and all roles. Units are recorded, never converted.

## Running it

`abm-gen` needs Python 3 with the `libclang` and `PyYAML` packages: `pip install -r
tools/abm-gen/requirements.txt` installs the versions CI uses, checked by hash.

```
python3 tools/abm-gen/abm_gen.py --out generated.h [--side units.yaml] [--include header.h ...]
    [--compile-commands build/compile_commands.json] header.h ... [-- compiler flags]
```

It asks the C++ compiler (`--cxx`, by default `$CXX`) for its include directories, which libclang needs,
and reports errors with the class and member concerned: a macro naming the wrong base, a class template, an
unknown entry in the side file, or a header that does not parse.

## The build

CMake regenerates the header when the model's headers or the side file change. For BioDynaMo this hooks into
`bdm_add_executable` during step 3.

## Testing

- **In abm-io's CI:** `tests/generator` holds a miniature BioDynaMo (its macros, its own types, classes with
  private, static and transient members, and a model in a header of its own) with the expected output. The
  Python tests compare the output and check every error; a C++ test compiles the generated header and checks
  that it describes a valid schema and drives the data visitors. `-DABMIO_BUILD_GENERATOR_TESTS=ON` builds
  them; the CI job "Generator (abm-gen)" runs them.
- **In BioDynaMo's CI (issue #10):** BioDynaMo's 11 agent and behaviour classes and the 51 in its demos and
  tests. The generated descriptions must match hand-checked ones, and the generated header must compile
  against BioDynaMo. Only there are BioDynaMo and ROOT installed.
