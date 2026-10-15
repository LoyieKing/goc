/* Reproducibility input: many functions, each in its own section, each
 * reading a different external array, so gocobj2goobj creates many GOT
 * slots from many text sections (their order used to follow map order). */
extern int ext_0[];
extern int ext_1[];
extern int ext_2[];
extern int ext_3[];
extern int ext_4[];
extern int ext_5[];
extern int ext_6[];
extern int ext_7[];
extern int ext_8[];
extern int ext_9[];
extern int ext_10[];
extern int ext_11[];
extern int ext_12[];
extern int ext_13[];
extern int ext_14[];
extern int ext_15[];
extern int ext_16[];
extern int ext_17[];
extern int ext_18[];
extern int ext_19[];
extern int ext_20[];
extern int ext_21[];
extern int ext_22[];
extern int ext_23[];
int read_0(int k) { return ext_0[k] + ext_1[k + 1]; }
int read_1(int k) { return ext_1[k] + ext_2[k + 1]; }
int read_2(int k) { return ext_2[k] + ext_3[k + 1]; }
int read_3(int k) { return ext_3[k] + ext_4[k + 1]; }
int read_4(int k) { return ext_4[k] + ext_5[k + 1]; }
int read_5(int k) { return ext_5[k] + ext_6[k + 1]; }
int read_6(int k) { return ext_6[k] + ext_7[k + 1]; }
int read_7(int k) { return ext_7[k] + ext_8[k + 1]; }
int read_8(int k) { return ext_8[k] + ext_9[k + 1]; }
int read_9(int k) { return ext_9[k] + ext_10[k + 1]; }
int read_10(int k) { return ext_10[k] + ext_11[k + 1]; }
int read_11(int k) { return ext_11[k] + ext_12[k + 1]; }
int read_12(int k) { return ext_12[k] + ext_13[k + 1]; }
int read_13(int k) { return ext_13[k] + ext_14[k + 1]; }
int read_14(int k) { return ext_14[k] + ext_15[k + 1]; }
int read_15(int k) { return ext_15[k] + ext_16[k + 1]; }
int read_16(int k) { return ext_16[k] + ext_17[k + 1]; }
int read_17(int k) { return ext_17[k] + ext_18[k + 1]; }
int read_18(int k) { return ext_18[k] + ext_19[k + 1]; }
int read_19(int k) { return ext_19[k] + ext_20[k + 1]; }
int read_20(int k) { return ext_20[k] + ext_21[k + 1]; }
int read_21(int k) { return ext_21[k] + ext_22[k + 1]; }
int read_22(int k) { return ext_22[k] + ext_23[k + 1]; }
int read_23(int k) { return ext_23[k] + ext_0[k + 1]; }
