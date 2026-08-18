#!/usr/bin/env python3
"""
生成 pgvector HNSW 优化的性能对比图表（SVG 格式，零依赖）
"""

import sys
from pathlib import Path

# 实验数据（从 benchmark_report.md 提取）
data_50k = {
    'ef_search': [40, 200],
    'configs': ['off', 'on'],
    'metrics': {
        40: {'off': {'avg': 1.725, 'p50': 1.663, 'p95': 2.621, 'p99': 2.827, 'recall': 0.376},
             'on':  {'avg': 1.559, 'p50': 1.526, 'p95': 1.976, 'p99': 2.470, 'recall': 0.376}},
        200: {'off': {'avg': 4.726, 'p50': 4.431, 'p95': 6.567, 'p99': 7.584, 'recall': 0.742},
              'on':  {'avg': 4.715, 'p50': 4.539, 'p95': 5.830, 'p99': 6.891, 'recall': 0.742}}
    }
}

data_200k = {
    'ef_search': [40, 200],
    'configs': ['off', 'on'],
    'metrics': {
        40: {'off': {'avg': 6.506, 'p50': 6.609, 'p95': 9.256, 'p99': 9.580, 'recall': 0.198},
             'on':  {'avg': 4.785, 'p50': 4.441, 'p95': 6.899, 'p99': 7.977, 'recall': 0.198}},
        200: {'off': {'avg': 22.952, 'p50': 21.765, 'p95': 27.524, 'p99': 48.092, 'recall': 0.478},
              'on':  {'avg': 15.415, 'p50': 14.527, 'p95': 20.530, 'p99': 21.227, 'recall': 0.478}}
    }
}

def generate_latency_comparison_svg(data, dataset_name, output_path):
    """生成延迟对比柱状图（avg, p95, p99）"""
    
    width, height = 800, 500
    margin = {'top': 60, 'right': 40, 'bottom': 80, 'left': 80}
    chart_width = width - margin['left'] - margin['right']
    chart_height = height - margin['top'] - margin['bottom']
    
    # 数据准备
    metrics_to_show = ['avg', 'p95', 'p99']
    ef_values = data['ef_search']
    configs = data['configs']
    
    # 计算 Y 轴最大值
    max_val = 0
    for ef in ef_values:
        for cfg in configs:
            for metric in metrics_to_show:
                max_val = max(max_val, data['metrics'][ef][cfg][metric])
    y_max = (int(max_val / 10) + 1) * 10
    
    # 颜色
    colors = {'off': '#ef4444', 'on': '#10b981'}
    
    # 开始生成 SVG
    svg_parts = [f'''<svg width="{width}" height="{height}" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <style>
      .title {{ font: bold 18px sans-serif; }}
      .axis-label {{ font: 14px sans-serif; }}
      .tick-label {{ font: 12px sans-serif; }}
      .legend-text {{ font: 12px sans-serif; }}
      .bar-label {{ font: 11px sans-serif; fill: white; }}
    </style>
  </defs>
  
  <!-- Background -->
  <rect width="{width}" height="{height}" fill="white"/>
  
  <!-- Title -->
  <text x="{width/2}" y="30" text-anchor="middle" class="title">
    {dataset_name} 延迟对比 (off vs on)
  </text>
  
  <!-- Y axis -->
  <line x1="{margin['left']}" y1="{margin['top']}" 
        x2="{margin['left']}" y2="{margin['top'] + chart_height}" 
        stroke="#999" stroke-width="2"/>
  <text x="{margin['left'] - 50}" y="{margin['top'] - 10}" class="axis-label">延迟 (ms)</text>
''']
    
    # Y axis ticks
    for i in range(6):
        y_val = y_max * i / 5
        y_pos = margin['top'] + chart_height - (chart_height * i / 5)
        svg_parts.append(f'''  <line x1="{margin['left'] - 5}" y1="{y_pos}" 
        x2="{margin['left']}" y2="{y_pos}" stroke="#999" stroke-width="1"/>
  <text x="{margin['left'] - 10}" y="{y_pos + 4}" text-anchor="end" class="tick-label">{y_val:.0f}</text>
  <line x1="{margin['left']}" y1="{y_pos}" 
        x2="{margin['left'] + chart_width}" y2="{y_pos}" 
        stroke="#eee" stroke-width="1" stroke-dasharray="3,3"/>
''')
    
    # X axis
    svg_parts.append(f'''  <line x1="{margin['left']}" y1="{margin['top'] + chart_height}" 
        x2="{margin['left'] + chart_width}" y2="{margin['top'] + chart_height}" 
        stroke="#999" stroke-width="2"/>
''')
    
    # Bars
    group_width = chart_width / (len(ef_values) * len(metrics_to_show))
    bar_width = group_width * 0.35
    
    bar_index = 0
    for ef in ef_values:
        for metric in metrics_to_show:
            x_base = margin['left'] + bar_index * group_width
            
            for i, cfg in enumerate(configs):
                value = data['metrics'][ef][cfg][metric]
                bar_height = (value / y_max) * chart_height
                x = x_base + i * bar_width + bar_width * 0.1
                y = margin['top'] + chart_height - bar_height
                
                svg_parts.append(f'''  <rect x="{x}" y="{y}" width="{bar_width * 0.8}" height="{bar_height}" 
        fill="{colors[cfg]}" opacity="0.9"/>
  <text x="{x + bar_width * 0.4}" y="{y - 5}" text-anchor="middle" class="bar-label" 
        fill="{colors[cfg]}" font-weight="bold">{value:.1f}</text>
''')
            
            # X tick label
            x_center = x_base + bar_width
            svg_parts.append(f'''  <text x="{x_center}" y="{margin['top'] + chart_height + 20}" 
        text-anchor="middle" class="tick-label">ef={ef}</text>
  <text x="{x_center}" y="{margin['top'] + chart_height + 40}" 
        text-anchor="middle" class="tick-label" font-weight="bold">{metric}</text>
''')
            
            bar_index += 1
    
    # Legend
    legend_x = margin['left'] + chart_width - 120
    legend_y = margin['top'] + 20
    svg_parts.append(f'''  <rect x="{legend_x}" y="{legend_y}" width="110" height="60" 
        fill="white" stroke="#ccc" stroke-width="1" rx="3"/>
  <rect x="{legend_x + 10}" y="{legend_y + 15}" width="20" height="15" fill="{colors['off']}"/>
  <text x="{legend_x + 35}" y="{legend_y + 27}" class="legend-text">off (官方)</text>
  <rect x="{legend_x + 10}" y="{legend_y + 35}" width="20" height="15" fill="{colors['on']}"/>
  <text x="{legend_x + 35}" y="{legend_y + 47}" class="legend-text">on (优化)</text>
''')
    
    svg_parts.append('</svg>')
    
    output_path.write_text('\n'.join(svg_parts), encoding='utf-8')
    print(f"[OK] generated {output_path.name}")

def generate_improvement_svg(data, dataset_name, output_path):
    """生成改善百分比图（p99 重点突出）"""
    
    width, height = 600, 400
    margin = {'top': 60, 'right': 40, 'bottom': 60, 'left': 100}
    chart_width = width - margin['left'] - margin['right']
    chart_height = height - margin['top'] - margin['bottom']
    
    # 计算改善百分比
    improvements = []
    for ef in data['ef_search']:
        for metric in ['avg', 'p95', 'p99']:
            off_val = data['metrics'][ef]['off'][metric]
            on_val = data['metrics'][ef]['on'][metric]
            improve = ((off_val - on_val) / off_val) * 100
            improvements.append({
                'label': f"ef={ef} {metric}",
                'value': improve,
                'highlight': (metric == 'p99' and ef == 200)
            })
    
    max_improve = max(item['value'] for item in improvements)
    x_max = (int(max_improve / 10) + 1) * 10
    
    svg_parts = [f'''<svg width="{width}" height="{height}" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <style>
      .title {{ font: bold 16px sans-serif; }}
      .bar-label-left {{ font: 12px sans-serif; text-anchor: end; }}
      .bar-label-right {{ font: bold 12px sans-serif; fill: white; }}
    </style>
  </defs>
  
  <rect width="{width}" height="{height}" fill="white"/>
  
  <text x="{width/2}" y="30" text-anchor="middle" class="title">
    {dataset_name} 延迟改善百分比
  </text>
  
  <line x1="{margin['left']}" y1="{margin['top']}" 
        x2="{margin['left']}" y2="{margin['top'] + chart_height}" 
        stroke="#999" stroke-width="2"/>
''']
    
    bar_height = chart_height / len(improvements) * 0.7
    
    for i, item in enumerate(improvements):
        y = margin['top'] + i * (chart_height / len(improvements)) + (chart_height / len(improvements) - bar_height) / 2
        bar_width_px = (item['value'] / x_max) * chart_width
        
        color = '#dc2626' if item['highlight'] else '#3b82f6'
        
        svg_parts.append(f'''  <text x="{margin['left'] - 10}" y="{y + bar_height/2 + 4}" class="bar-label-left">
    {item['label']}
  </text>
  <rect x="{margin['left']}" y="{y}" width="{bar_width_px}" height="{bar_height}" 
        fill="{color}" opacity="0.9"/>
  <text x="{margin['left'] + bar_width_px + 10}" y="{y + bar_height/2 + 4}" 
        class="bar-label-right" fill="{color}">-{item['value']:.1f}%</text>
''')
    
    svg_parts.append('</svg>')
    
    output_path.write_text('\n'.join(svg_parts), encoding='utf-8')
    print(f"[OK] generated {output_path.name}")

if __name__ == '__main__':
    output_dir = Path('DOC/优化/images')
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # 生成 4 张图
    generate_latency_comparison_svg(data_50k, '50k 行数据集', 
                                   output_dir / 'latency_50k.svg')
    generate_latency_comparison_svg(data_200k, '200k 行数据集', 
                                   output_dir / 'latency_200k.svg')
    generate_improvement_svg(data_50k, '50k 行', 
                            output_dir / 'improvement_50k.svg')
    generate_improvement_svg(data_200k, '200k 行', 
                            output_dir / 'improvement_200k.svg')
    
    print(f"\nAll charts generated in {output_dir}/")
    print("Reference in README.md:")
    print("![latency 50k](DOC/优化/images/latency_50k.svg)")
