//
//  ChatView.swift
//  Firefly
//
//  Created by Jake Holland on 2/19/26.
//

import SwiftUI

struct ChatView: View {
    let chat: Chat
    
    var body: some View {
        ForEach(chat.messages) { message in
            VStack(alignment: . leading) {                
                HStack(alignment: .bottom)  {
                    if (!message.sender.isMe) {
                        ZStack(alignment: .center) {
                            Circle()
                                .foregroundColor(.green)
                                .frame(width: 32, height: 32, alignment: .center)
                                .padding(.bottom, 0)
                            
                            Text(message.sender.initials)
                                .font(.headline)
                        }
                        
                    }
                    
                    HStack(alignment: .top) {
                        Text(message.text)
                            .padding()
                    }
                    .background(message.sender.isMe ? .blue : .gray)
                    .cornerRadius(20)
                }
                .frame(maxWidth: 360, alignment: message.sender.isMe ? .trailing : .leading)
            }
        }
    }
}

#Preview {
    ChatView(chat: MockData.chat1)
}
