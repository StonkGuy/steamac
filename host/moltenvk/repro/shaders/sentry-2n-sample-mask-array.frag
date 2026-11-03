#version 450
// Signed SPIR-V masks and an ordinary (non-builtin) array parameter.
int read_word(in int words[1])
{
    return words[0];
}
void write_mask()
{
    int words[1] = int[1](gl_SampleMaskIn[0]);
    gl_SampleMask[0] = floatBitsToInt(intBitsToFloat(read_word(words)));
}
void forward_mask()
{
    write_mask();
}
void main()
{
    forward_mask();
}
