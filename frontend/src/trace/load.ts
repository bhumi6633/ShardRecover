import {validateTrace} from './validate'
export function parseTrace(text:string){let value:unknown;try{value=JSON.parse(text)}catch{throw new Error('The selected file is not valid JSON.')}return validateTrace(value)}
export async function loadTraceFile(file:File){try{return parseTrace(await file.text())}catch(error){throw error instanceof Error?error:new Error('Unable to read the selected trace file.')}}
