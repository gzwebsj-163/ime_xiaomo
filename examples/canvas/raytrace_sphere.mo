# xiaomo 光线追踪: 3D 球体 + 漫反射光照
# 用法: ./xiaomo run examples/canvas/raytrace_sphere.mo

# ======== 光线追踪渲染 ========
# 球心在原点 (0,0,0), 半径 5, 相机在 (0,0,15)
# 光照来自右上前方 (1,-1,1): 照亮球体正面
void img : int = render_raytrace(200, 200, 0.0, 0.0, 0.0, 5.0, 0.0, 0.0, 15.0, 1.0, -1.0, 1.0, 0.15, 0.85)
void _i : int = nd_info(${img})

# ======== 输出 PGM 图片数据 ========
>> print >> "%!PGM_BEGIN 200 200"
void i : int = 0
while ${i} < 200:
    void row_i : int = nd_slice(${img}, [${i},0], [${i}+1,200], [1,1])
    void row_arr : int = tensor_to_array(${row_i})
    >> print >> ${row_arr}
    void i : int = ${i} + 1
>> print >> "%!PGM_END"

# 统计: 平均亮度
void sum_img : int = nd_sum(${img}, -1)
void avg : int = nd_div(${sum_img}, 40000.0)
void avg_print : int = tensor_to_array(${avg})
>> print >> "Average brightness: " >> ${avg_print}