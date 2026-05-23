    .text
    .globl rv_cbo_inval
    .type  rv_cbo_inval, @function
rv_cbo_inval:
    cbo.inval 0(a0)
    ret