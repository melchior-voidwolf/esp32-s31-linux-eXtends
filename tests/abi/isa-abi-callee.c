/* External float callee: prevents inlining and preserves the selected ABI. */
float abi_float_step(float acc, float factor, float increment)
{
    return acc * factor + increment;
}
