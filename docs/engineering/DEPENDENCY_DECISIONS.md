# Dependency decisions

1. Keep ELFIO for metadata parsing. Validate PS2-specific admission in the shared parser. ELFIO's ability to read an ELF header in another byte order does not imply that Zmbor can execute its payload.
2. Preserve Ghidra's optional external boundary. Feed the existing candidate validator and record the precision of exported entries before enabling it more broadly.
3. Keep hardware references and SMT tooling as development inputs. Attribute each expected result and record its revision and evidence class.
4. Preserve software GS and gate optional Vulkan integration on frame and VRAM equality and repeatable timings.
5. Pin moving dependencies in a separate build-reproducibility change. Studio currently fetches several branch heads. Record dependency object IDs from each successful baseline.

See [candidate evaluation](../../research/OPEN_SOURCE_TECHNOLOGY_EVALUATION.md) and [earlier feasibility experiments](../research/INTEGRATION_FEASIBILITY_MATRIX.md).
