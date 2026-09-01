import type{EdgeTrace,Trace}from'../trace/types'
export const formatOffset=(value:number,width=4)=>`0x${value.toString(16).toUpperCase().padStart(width,'0')}`
export function pathEdges(trace:Trace){return trace.selected_path.slice(1).map((to,index)=>trace.edges.find(edge=>edge.from===trace.selected_path[index]&&edge.to===to)).filter((edge):edge is EdgeTrace=>Boolean(edge))}
export function joinIndex(trace:Trace,edge:EdgeTrace){return pathEdges(trace).findIndex(item=>item.from===edge.from&&item.to===edge.to)}
export function adjacentJoin(trace:Trace,edge:EdgeTrace,delta:number,mismatchesOnly=false){const edges=pathEdges(trace),start=edges.findIndex(item=>item.from===edge.from&&item.to===edge.to);for(let i=start+delta;i>=0&&i<edges.length;i+=delta)if(!mismatchesOnly||edges[i].mismatches>0)return edges[i]}
