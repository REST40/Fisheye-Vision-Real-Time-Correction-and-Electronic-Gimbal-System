// jpeg_open 存储适配层：同步读、独立写端口，保持一拍读取延迟。
module rfdp12288x64 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [13:0]      AA,
    input  wire [13:0]      AB,
    input  wire [63:0]      DB,

    output reg  [63:0]      QA
);
    reg [63:0] memory [0:12287];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp512x96 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [8:0]       AA,
    input  wire [8:0]       AB,
    input  wire [95:0]      DB,

    output reg  [95:0]      QA
);
    reg [95:0] memory [0:511];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp8x12 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [2:0]       AA,
    input  wire [2:0]       AB,
    input  wire [11:0]      DB,

    output reg  [11:0]      QA
);
    reg [11:0] memory [0:7];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp16x12 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [3:0]       AA,
    input  wire [3:0]       AB,
    input  wire [11:0]      DB,

    output reg  [11:0]      QA
);
    reg [11:0] memory [0:15];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp512x32 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [8:0]       AA,
    input  wire [8:0]       AB,
    input  wire [31:0]      DB,

    output reg  [31:0]      QA
);
    reg [31:0] memory [0:511];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp512x36 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [8:0]       AA,
    input  wire [8:0]       AB,
    input  wire [35:0]      DB,

    output reg  [35:0]      QA
);
    reg [35:0] memory [0:511];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

module rfdp1024x96 (
    input  wire             CLKA,
    input  wire             CLKB,

    input  wire             CENA,
    input  wire             CENB,

    input  wire [9:0]       AA,
    input  wire [9:0]       AB,
    input  wire [95:0]      DB,

    output reg  [95:0]      QA
);
    reg [95:0] memory [0:1023];
    always @(posedge CLKB) if (!CENB) memory[AB] <= DB;
    always @(posedge CLKA) if (!CENA) QA <= memory[AA];
endmodule

