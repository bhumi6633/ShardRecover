import type { Trace } from './types'
const object=(v:unknown):v is Record<string,unknown>=>typeof v==='object'&&v!==null&&!Array.isArray(v)
export function validateTrace(value:unknown):Trace {
  if(!object(value)) throw new Error('Trace root must be an object.')
  if(value.schema_version!==1) throw new Error(`Unsupported trace schema version: ${String(value.schema_version)}. This frontend currently supports version 1.`)
  for(const field of ['fragments','edges','selected_path'] as const) if(!Array.isArray(value[field])) throw new Error(`Trace field "${field}" must be an array.`)
  if(!object(value.reconstruction)||!object(value.configuration)||!object(value.graph_stats)||!object(value.format)) throw new Error('Trace is missing required summary objects.')
  const fragments=value.fragments as unknown[], edges=value.edges as unknown[], path=value.selected_path as unknown[]
  const ids=new Set<number>()
  for(const fragment of fragments){if(!object(fragment)||typeof fragment.id!=='number'||typeof fragment.name!=='string'||typeof fragment.size!=='number')throw new Error('Invalid fragment metadata.');ids.add(fragment.id)}
  for(const id of path) if(typeof id!=='number'||!ids.has(id)) throw new Error(`Selected path references unknown fragment ${String(id)}.`)
  for(const edge of edges) if(!object(edge)||typeof edge.from!=='number'||typeof edge.to!=='number'||!ids.has(edge.from)||!ids.has(edge.to)) throw new Error('Edge references an unknown fragment.')
  return value as unknown as Trace
}
