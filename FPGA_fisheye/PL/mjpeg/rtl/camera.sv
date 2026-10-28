`include "jpeg_global.v"

// 将可停顿的像素输入重排为 jpeg_open 的 8 行块缓冲布局。
// 两个 8 行存储区轮换，填充下一条带与当前条带的 DCT 处理重叠。
module camera (
    input  wire                     clk,

    input  wire                     cam_clk,

    input  wire                     rstn,
    input  wire                     rstn_cam,

    input  wire                     encoder_active,
    input  wire                     core_idle,
    input  wire                     cam_vsync_i,
    input  wire                     cam_href_i,
    input  wire [`W_CAMD_I:0]        cam_data_i,
    input  wire [`W_PW:0]            PicWidth_i,
    input  wire [`W_PH:0]            PicHeight_i,

    output wire                     pixel_ready,
    output reg                      cam_pic_start_f,
    output reg                      camfifo_o_f,
    output reg                      cenb_cam,
    output wire [`W_WCAMBUF:0]       wenb_cam,
    output reg  [`W_ACAMBUF:0]       ab_cam,
    output reg  [63:0]               db_cam
);
    reg [10:0] x;
    reg [10:0] y;
    reg [63:0] buf_y;
    reg [63:0] buf_u;
    reg [63:0] buf_v;
    reg [12:0] group_addr;
    reg [2:0]  write_phase;
    reg [10:0] guard_count;
    reg        strip_wait;
    reg        strip_last;
    reg        vsync_d;

    assign wenb_cam = 0;
    assign pixel_ready = encoder_active && !cam_vsync_i &&
                         (guard_count == 0) && !strip_wait &&
                         (write_phase == 0) && (y < PicHeight_i);

    always @(posedge clk) begin
        if (!rstn) begin
            x <= 0;
            y <= 0;
            buf_y <= 0;
            buf_u <= 0;
            buf_v <= 0;
            group_addr <= 0;
            write_phase <= 0;
            guard_count <= 0;
            strip_wait <= 0;
            strip_last <= 0;
            vsync_d <= 1;
            cam_pic_start_f <= 0;
            camfifo_o_f <= 0;
            cenb_cam <= 1;
            ab_cam <= 0;
            db_cam <= 0;
        end else begin
            vsync_d <= cam_vsync_i;
            cam_pic_start_f <= 0;
            camfifo_o_f <= 0;
            cenb_cam <= 1;
            if (guard_count != 0) guard_count <= guard_count - 1'b1;
            if (vsync_d && !cam_vsync_i) begin
                cam_pic_start_f <= 1;
                guard_count <= 1024; // 先发送完整 JPEG 文件头。
            end
            // 条带写入完成后等待前一条带处理结束，再发布新的条带。
            // 发布后立即允许填充另一半存储区；保护计数覆盖控制传播延迟。
            if (strip_wait && guard_count == 0 && core_idle) begin
                camfifo_o_f <= 1;
                strip_wait <= 0;
                guard_count <= 16;
            end
            if (cam_href_i && pixel_ready) begin
                buf_y <= (buf_y << 8) | {56'd0, cam_data_i[7:0]};
                buf_u <= (buf_u << 8) | {56'd0, cam_data_i[15:8]};
                buf_v <= (buf_v << 8) | {56'd0, cam_data_i[23:16]};
                if (x[2:0] == 7) begin
                    group_addr <= {1'b0, y[3:0], 8'b0} + (x >> 3);
                    write_phase <= 1;
                    strip_last <= (x == PicWidth_i - 1) && (y[2:0] == 7);
                end
                if (x == PicWidth_i - 1) begin
                    x <= 0;
                    y <= y + 1'b1;
                end else x <= x + 1'b1;
            end
            case (write_phase)
                1: begin
                    cenb_cam <= 0;
                    ab_cam <= group_addr;
                    db_cam <= buf_y;
                    write_phase <= 2;
                end
                2: begin
                    cenb_cam <= 0;
                    ab_cam <= group_addr + 4096;
                    db_cam <= buf_u;
                    write_phase <= 3;
                end
                3: begin
                    cenb_cam <= 0;
                    ab_cam <= group_addr + 8192;
                    db_cam <= buf_v;
                    write_phase <= 4;
                end
                4: begin
                    write_phase <= 0;
                    if (strip_last) begin
                        strip_wait <= 1;
                        guard_count <= 16;
                    end
                end
            endcase
        end
    end
endmodule
